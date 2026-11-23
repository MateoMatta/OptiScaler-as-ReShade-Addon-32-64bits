#include "pch.h"
#include "dllmain.h"
#if defined(OPTISCALER_RESHADE_ADDON)
#include <reshade.hpp>
#include "AddonResourceSnapshot.h"
#endif

#include "Util.h"
#include "Config.h"
#include "Logger.h"
#include "resource.h"
#include "DllNames.h"
#include "BuildInfo.h"
#include "misc/CrashDumps.h"
#include "misc/GameConfigs.h"
#include "plugins/PluginApi.h"

#include "proxies/Dxgi_Proxy.h"
#include "proxies/Kernel32_Proxy.h"
#include "proxies/KernelBase_Proxy.h"
#include "proxies/Ntdll_Proxy.h"
#include <proxies/IGDExt_Proxy.h>

#include <proxies/XeSS_Proxy.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/XeLL_Proxy.h>
#include <proxies/NVNGX_Proxy.h>
#include <proxies/FfxApi_Proxy.h>

#include "inputs/FSR2_Dx11.h"
#include "inputs/FSR2_Dx12.h"
#include "inputs/FSR2_Vk.h"
#include "inputs/FSR3_Dx12.h"
#include "inputs/FG/FSR3_Dx12_FG.h"
#include "inputs/FG/XeFG_Inputs_Dx12.h"

#include <hooks/Amdxc64_Hooks.h>
#include <hooks/Dxgi_Hooks.h>
#include <hooks/D3D11_Hooks.h>
#include <hooks/D3D12_Hooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <hooks/Ntdll_Hooks.h>
#include <hooks/Kernel_Hooks.h>
#include <hooks/Gdi32_Hooks.h>
#include <hooks/Wintrust_Hooks.h>
#include <hooks/Crypt32_Hooks.h>
#include <hooks/Advapi32_Hooks.h>
#include <hooks/Streamline_Hooks.h>

#include <nvapi/NvApiHooks.h>

#include "spoofing/User32_Spoofing.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <utility>
#include <magic_enum.hpp>
#include <version_check.h>
#include <misc/IdentifyGpu.h>
#include <sha1/sha1.hpp>

#if defined(OPTISCALER_RESHADE_ADDON)
extern "C" __declspec(dllexport) const char *NAME = "OptiScaler ReShade Integration Probe";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Experimental ReShade lifecycle and presentation diagnostics; upscaling is not integrated yet.";

static SRWLOCK _addonLogLock = SRWLOCK_INIT;
static CONDITION_VARIABLE _addonLogReady = CONDITION_VARIABLE_INIT;
static CONDITION_VARIABLE _addonLogSpace = CONDITION_VARIABLE_INIT;
static std::array<wchar_t, 32768> _addonLogPath {};
static std::atomic<uint64_t> _addonPresentCount { 0 };
static std::atomic<uint64_t> _addonBindCount { 0 };
static std::atomic<uint64_t> _addonDescriptorCount { 0 };
static std::atomic<uint64_t> _addonAuxDescriptorCount { 0 };
static std::atomic<uint64_t> _addonDroppedLogs { 0 };
static std::atomic<uint64_t> _addonDroppedDebugLogs { 0 };
static std::atomic<uint64_t> _addonDroppedImportantLogs { 0 };
static std::atomic<uint64_t> _addonLogQueueWaits { 0 };
static std::atomic<uint64_t> _addonLogTruncated { 0 };
static std::atomic<size_t> _addonLogHighWater { 0 };
static constexpr size_t AddonLogCapacity = 4096;
struct AddonLogLine
{
    char text[1280] {};
    DWORD length = 0;
    int level = 0;
};
static std::array<AddonLogLine, AddonLogCapacity> _addonLogQueue {};
static size_t _addonLogHead = 0;
static size_t _addonLogCount = 0;
static bool _addonLogStopping = false;
static HANDLE _addonLogThread = nullptr;
static HANDLE _addonLogFile = INVALID_HANDLE_VALUE;
static bool _addonRegistered = false;
static bool _addonLogToFile = true;
static bool _addonLogToDebug = false;
static bool _addonLogToConsole = false;
static int _addonLogLevel = 0;
static uint64_t _addonDetailPresentStart = 0;
static bool _addonDetailConfigError = false;
static bool _addonAllTargetBindings = false;
static bool _addonShaderBindings = false;
static bool _addonResourceLifetimes = false;
static SRWLOCK _addonResourceLock = SRWLOCK_INIT;
using AddonResourceKey = std::pair<uintptr_t, uint64_t>;
static std::map<AddonResourceKey, uint64_t> _addonResourceGenerations;
static uint64_t _addonNextResourceGeneration = 0;
static uint64_t _addonResourceEventSequence = 0;
static constexpr uint64_t AddonDetailPresentCount = 8;

static bool ConfigureAddonResourceLifetimes()
{
    wchar_t value[8] {};
    const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_PROBE_RESOURCE_LIFETIMES", value, _countof(value));
    if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return true;
    if (size != 1 || value[0] != L'1' || _addonLogLevel > 1 || !_addonLogToFile)
    {
        OutputDebugStringA("OptiScaler ReShade log: resource lifetimes requires value 1, file logging and LogLevel <= 1\n");
        return false;
    }
    _addonResourceLifetimes = true;
    return true;
}


static void ConfigureAddonDetailWindow()
{
    _addonDetailPresentStart = 0;
    _addonDetailConfigError = false;
    wchar_t value[32] {};
    const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_PROBE_DETAIL_START", value, _countof(value));
    if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return;
    if (size == 0 || size >= _countof(value) || value[0] < L'1' || value[0] > L'9')
    {
        _addonDetailConfigError = true;
        OutputDebugStringA("OptiScaler ReShade log: invalid OPTISCALER_PROBE_DETAIL_START\n");
        return;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(value, &end, 10);
    if (errno == ERANGE || *end != L'\0' || parsed > UINT64_MAX - AddonDetailPresentCount)
    {
        _addonDetailConfigError = true;
        OutputDebugStringA("OptiScaler ReShade log: invalid OPTISCALER_PROBE_DETAIL_START\n");
        return;
    }
    _addonDetailPresentStart = parsed;
}

static bool ConfigureAddonAllTargetBindings()
{
    _addonAllTargetBindings = false;
    wchar_t value[8] {};
    const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_PROBE_ALL_TARGET_BINDINGS", value, _countof(value));
    if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return true;
    if (size != 1 || value[0] != L'1')
    {
        OutputDebugStringA("OptiScaler ReShade log: OPTISCALER_PROBE_ALL_TARGET_BINDINGS must be 1\n");
        return false;
    }
    if (_addonLogLevel > 1 || !_addonLogToFile)
    {
        OutputDebugStringA("OptiScaler ReShade log: all target bindings requires file logging and LogLevel <= 1\n");
        return false;
    }
    _addonAllTargetBindings = true;
    return true;
}

static bool ConfigureAddonShaderBindings()
{
    _addonShaderBindings = false;
    wchar_t value[8] {};
    const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_PROBE_SHADER_BINDINGS", value, _countof(value));
    if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return true;
    if (size != 1 || value[0] != L'1' || _addonLogLevel > 1 || !_addonLogToFile)
    {
        OutputDebugStringA("OptiScaler ReShade log: shader bindings requires value 1, file logging and LogLevel <= 1\n");
        return false;
    }
    _addonShaderBindings = true;
    return true;
}

static bool InAddonDetailWindow(uint64_t present)
{
    return _addonDetailPresentStart != 0 && present >= _addonDetailPresentStart &&
           present - _addonDetailPresentStart < AddonDetailPresentCount;
}

static DWORD WINAPI AddonLogWorker(void*)
{
    std::array<AddonLogLine, 64> batch {};
    bool fileErrorReported = false;
    bool consoleErrorReported = false;
    for (;;)
    {
        AcquireSRWLockExclusive(&_addonLogLock);
        while (_addonLogCount == 0 && !_addonLogStopping)
        {
            if (!SleepConditionVariableSRW(&_addonLogReady, &_addonLogLock, INFINITE, 0))
            {
                OutputDebugStringA("OptiScaler ReShade log: worker wait failed\n");
                _addonLogStopping = true;
                WakeAllConditionVariable(&_addonLogSpace);
            }
        }
        const size_t count = (std::min)(_addonLogCount, batch.size());
        for (size_t i = 0; i < count; ++i)
        {
            batch[i] = _addonLogQueue[_addonLogHead];
            _addonLogHead = (_addonLogHead + 1) % AddonLogCapacity;
        }
        _addonLogCount -= count;
        if (count != 0)
            WakeAllConditionVariable(&_addonLogSpace);
        const bool finished = _addonLogStopping && count == 0;
        ReleaseSRWLockExclusive(&_addonLogLock);
        if (finished)
            break;

        char output[64 * 1280];
        DWORD length = 0;
        for (size_t i = 0; i < count; ++i)
        {
            memcpy(output + length, batch[i].text, batch[i].length);
            length += batch[i].length;
            if (_addonLogToDebug)
                OutputDebugStringA(batch[i].text);
            if (_addonLogToConsole && batch[i].level >= 2 && !consoleErrorReported)
            {
                HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
                DWORD written = 0;
                if (console == nullptr || console == INVALID_HANDLE_VALUE ||
                    !WriteFile(console, batch[i].text, batch[i].length, &written, nullptr) ||
                    written != batch[i].length)
                {
                    OutputDebugStringA("OptiScaler ReShade log: console write failed or no console is available\n");
                    consoleErrorReported = true;
                }
            }
        }
        if (_addonLogFile != INVALID_HANDLE_VALUE && !fileErrorReported)
        {
            DWORD written = 0;
            if (!WriteFile(_addonLogFile, output, length, &written, nullptr) || written != length)
            {
                char error[128] {};
                sprintf_s(error, "OptiScaler ReShade log: batch write failed, error=%lu\n", GetLastError());
                OutputDebugStringA(error);
                fileErrorReported = true;
            }
        }
    }

    char summary[256] {};
    const int size = sprintf_s(summary, "event=logger_shutdown dropped=%llu file_write_failed=%u dropped_debug=%llu dropped_info_or_error=%llu queue_waits=%llu queue_high_water=%llu truncated=%llu\r\n",
                               static_cast<unsigned long long>(_addonDroppedLogs.load(std::memory_order_relaxed)),
                               fileErrorReported ? 1u : 0u,
                               static_cast<unsigned long long>(_addonDroppedDebugLogs.load(std::memory_order_relaxed)),
                               static_cast<unsigned long long>(_addonDroppedImportantLogs.load(std::memory_order_relaxed)),
                               static_cast<unsigned long long>(_addonLogQueueWaits.load(std::memory_order_relaxed)),
                               static_cast<unsigned long long>(_addonLogHighWater.load(std::memory_order_relaxed)),
                               static_cast<unsigned long long>(_addonLogTruncated.load(std::memory_order_relaxed)));
    if (_addonLogFile != INVALID_HANDLE_VALUE && size > 0)
    {
        DWORD written = 0;
        if (!WriteFile(_addonLogFile, summary, static_cast<DWORD>(size), &written, nullptr) ||
            written != static_cast<DWORD>(size))
            OutputDebugStringA("OptiScaler ReShade log: failed to write shutdown summary\n");
        CloseHandle(_addonLogFile);
        _addonLogFile = INVALID_HANDLE_VALUE;
    }
    if (_addonDroppedLogs.load(std::memory_order_relaxed) != 0 || fileErrorReported)
        OutputDebugStringA(summary);
    return 0;
}

static bool StartAddonLogger()
{
    _addonLogHead = 0;
    _addonLogCount = 0;
    _addonLogStopping = false;
    _addonDroppedLogs.store(0, std::memory_order_relaxed);
    _addonDroppedDebugLogs.store(0, std::memory_order_relaxed);
    _addonDroppedImportantLogs.store(0, std::memory_order_relaxed);
    _addonLogQueueWaits.store(0, std::memory_order_relaxed);
    _addonLogHighWater.store(0, std::memory_order_relaxed);
    _addonLogTruncated.store(0, std::memory_order_relaxed);
    _addonPresentCount.store(0, std::memory_order_relaxed);
    _addonBindCount.store(0, std::memory_order_relaxed);
    _addonDescriptorCount.store(0, std::memory_order_relaxed);
    _addonAuxDescriptorCount.store(0, std::memory_order_relaxed);
    if (_addonLogToFile)
    {
        _addonLogFile = CreateFileW(_addonLogPath.data(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (_addonLogFile == INVALID_HANDLE_VALUE)
        {
            char error[128] {};
            sprintf_s(error, "OptiScaler ReShade log: open failed, error=%lu\n", GetLastError());
            OutputDebugStringA(error);
            return false;
        }
    }
    _addonLogThread = CreateThread(nullptr, 0, AddonLogWorker, nullptr, 0, nullptr);
    if (_addonLogThread == nullptr)
    {
        OutputDebugStringA("OptiScaler ReShade log: failed to start worker\n");
        if (_addonLogFile != INVALID_HANDLE_VALUE)
        {
            CloseHandle(_addonLogFile);
            _addonLogFile = INVALID_HANDLE_VALUE;
        }
        return false;
    }
    return true;
}

static void StopAddonLogger()
{
    if (_addonLogThread == nullptr)
        return;
    AcquireSRWLockExclusive(&_addonLogLock);
    _addonLogStopping = true;
    WakeConditionVariable(&_addonLogReady);
    WakeAllConditionVariable(&_addonLogSpace);
    ReleaseSRWLockExclusive(&_addonLogLock);
    if (WaitForSingleObject(_addonLogThread, INFINITE) != WAIT_OBJECT_0)
        OutputDebugStringA("OptiScaler ReShade log: worker wait failed\n");
    CloseHandle(_addonLogThread);
    _addonLogThread = nullptr;
}

static void RecordAddonDroppedLog(int level)
{
    _addonDroppedLogs.fetch_add(1, std::memory_order_relaxed);
    (level == 1 ? _addonDroppedDebugLogs : _addonDroppedImportantLogs).fetch_add(1, std::memory_order_relaxed);
}

static void WriteAddonLog(int level, const char* format, va_list args)
{
    if (level < _addonLogLevel)
        return;

    char message[1024] {};
    if (_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args) < 0)
    {
        _addonLogTruncated.fetch_add(1, std::memory_order_relaxed);
        OutputDebugStringA("OptiScaler ReShade log: message formatting failed or was truncated\n");
    }

    SYSTEMTIME time {};
    GetLocalTime(&time);

    char line[1280] {};
    const int prefixLength = sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u][pid=%lu tid=%lu] ",
                                      static_cast<unsigned int>(time.wYear), static_cast<unsigned int>(time.wMonth),
                                      static_cast<unsigned int>(time.wDay), static_cast<unsigned int>(time.wHour),
                                      static_cast<unsigned int>(time.wMinute), static_cast<unsigned int>(time.wSecond),
                                      static_cast<unsigned int>(time.wMilliseconds), GetCurrentProcessId(),
                                      GetCurrentThreadId());
    if (prefixLength <= 0)
        return;

    const size_t messageLength = strnlen_s(message, sizeof(message));
    const size_t remaining = sizeof(line) - static_cast<size_t>(prefixLength) - 3;
    const size_t copiedLength = (std::min)(messageLength, remaining);
    memcpy(line + prefixLength, message, copiedLength);
    line[prefixLength + copiedLength] = '\r';
    line[prefixLength + copiedLength + 1] = '\n';
    line[prefixLength + copiedLength + 2] = '\0';

    AcquireSRWLockExclusive(&_addonLogLock);
    while (_addonLogCount == AddonLogCapacity && !_addonLogStopping)
    {
        _addonLogQueueWaits.fetch_add(1, std::memory_order_relaxed);
        if (!SleepConditionVariableSRW(&_addonLogSpace, &_addonLogLock, INFINITE, 0))
        {
            OutputDebugStringA("OptiScaler ReShade log: queue wait failed\n");
            RecordAddonDroppedLog(level);
            ReleaseSRWLockExclusive(&_addonLogLock);
            return;
        }
    }
    if (_addonLogStopping)
        RecordAddonDroppedLog(level);
    else
    {
        auto& entry = _addonLogQueue[(_addonLogHead + _addonLogCount) % AddonLogCapacity];
        entry.length = static_cast<DWORD>(prefixLength + copiedLength + 2);
        entry.level = level;
        memcpy(entry.text, line, entry.length + 1);
        ++_addonLogCount;
        if (_addonLogCount > _addonLogHighWater.load(std::memory_order_relaxed))
            _addonLogHighWater.store(_addonLogCount, std::memory_order_relaxed);
        WakeConditionVariable(&_addonLogReady);
    }
    ReleaseSRWLockExclusive(&_addonLogLock);
}

static void LogAddonEvent(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    WriteAddonLog(strstr(format, "error=") != nullptr ? 4 : 2, format, args);
    va_end(args);
}

static void LogAddonDebug(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    WriteAddonLog(1, format, args);
    va_end(args);
}

static bool InitializeAddonLogPath(HMODULE module)
{
    std::array<wchar_t, 32768> modulePath {};
    const DWORD length = GetModuleFileNameW(module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size())
        return false;

    wchar_t* separator = wcsrchr(modulePath.data(), L'\\');
    if (separator == nullptr)
        separator = wcsrchr(modulePath.data(), L'/');
    if (separator == nullptr)
        return false;

    *separator = L'\0';
    const std::filesystem::path directory(modulePath.data());
    const auto iniPath = directory / L"OptiScaler.ini";
    auto readSetting = [&](const wchar_t* key) {
        std::array<wchar_t, 32768> value {};
        GetPrivateProfileStringW(L"Log", key, L"auto", value.data(), static_cast<DWORD>(value.size()),
                                 iniPath.c_str());
        return std::wstring(value.data());
    };
    auto readBool = [&](const wchar_t* key, bool defaultValue) {
        const auto value = readSetting(key);
        if (_wcsicmp(value.c_str(), L"auto") == 0 || value.empty())
            return defaultValue;
        if (_wcsicmp(value.c_str(), L"true") == 0 || value == L"1")
            return true;
        if (_wcsicmp(value.c_str(), L"false") == 0 || value == L"0")
            return false;
        OutputDebugStringW((std::wstring(L"OptiScaler ReShade log: invalid boolean setting ") + key + L"\n").c_str());
        return defaultValue;
    };
    _addonLogToFile = readBool(L"LogToFile", true);
    _addonLogToDebug = readBool(L"LogToDebug", false);
    _addonLogToConsole = readBool(L"LogToConsole", false);
    const auto level = readSetting(L"LogLevel");
    if (_wcsicmp(level.c_str(), L"auto") != 0 && !level.empty())
    {
        wchar_t* end = nullptr;
        const long parsed = wcstol(level.c_str(), &end, 10);
        if (end != level.c_str() && *end == L'\0' && parsed >= 0 && parsed <= 4)
            _addonLogLevel = static_cast<int>(parsed);
        else
            OutputDebugStringA("OptiScaler ReShade log: invalid LogLevel, using trace\n");
    }
    const auto custom = readSetting(L"LogFileName");
    std::filesystem::path path = custom.empty() || _wcsicmp(custom.c_str(), L"auto") == 0
                                     ? directory / L"OptiScaler-ReShade.log"
                                     : std::filesystem::path(custom);
    if (path.is_relative())
        path = directory / path;
    if (!custom.empty() && _wcsicmp(custom.c_str(), L"auto") != 0)
        path = path.parent_path() / (path.stem().wstring() + L"-ReShade.log");
    if (!readBool(L"SingleFile", true))
    {
        LARGE_INTEGER counter {};
        if (!QueryPerformanceCounter(&counter))
        {
            OutputDebugStringA("OptiScaler ReShade log: unable to generate unique log filename\n");
            return false;
        }
        path = path.parent_path() / (path.stem().wstring() + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                     std::to_wstring(counter.QuadPart) + L".log");
    }
    if (wcsncpy_s(_addonLogPath.data(), _addonLogPath.size(), path.c_str(), _TRUNCATE) != 0)
        return false;

    if (readBool(L"LogToNGX", false) || readBool(L"CreateDumps", false) ||
        readBool(L"OpenConsole", false))
        OutputDebugStringA("OptiScaler ReShade log: NGX, dumps and console creation are not supported\n");
    return true;
}

static const char* GetAddonApiName(reshade::api::device_api api)
{
    switch (api)
    {
    case reshade::api::device_api::d3d9:
        return "d3d9";
    case reshade::api::device_api::d3d10:
        return "d3d10";
    case reshade::api::device_api::d3d11:
        return "d3d11";
    case reshade::api::device_api::d3d12:
        return "d3d12";
    case reshade::api::device_api::opengl:
        return "opengl";
    case reshade::api::device_api::vulkan:
        return "vulkan";
    default:
        return "unknown";
    }
}

static bool StartAddonTiming(LARGE_INTEGER& start)
{
    if (QueryPerformanceCounter(&start))
        return true;

    LogAddonEvent("error=performance_counter_start_failed");
    return false;
}

static bool GetAddonDurationMicroseconds(const LARGE_INTEGER& start, unsigned long long& duration)
{
    LARGE_INTEGER end {};
    LARGE_INTEGER frequency {};
    if (!QueryPerformanceCounter(&end))
    {
        LogAddonEvent("error=performance_counter_end_failed");
        return false;
    }
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0)
    {
        LogAddonEvent("error=performance_counter_frequency_unavailable");
        return false;
    }

    const uint64_t ticks = static_cast<uint64_t>(end.QuadPart - start.QuadPart);
    duration = static_cast<unsigned long long>((ticks * 1000000ull) /
                                               static_cast<uint64_t>(frequency.QuadPart));
    return true;
}

struct AddonBackBufferSnapshot
{
    uint32_t index = 0;
    reshade::api::resource resource {};
    reshade::api::resource_desc desc {};
};

static AddonBackBufferSnapshot InspectAddonBackBuffer(reshade::api::device* device,
                                                       reshade::api::swapchain* swapchain, uint32_t index)
{
    AddonBackBufferSnapshot snapshot {};
    snapshot.index = index;
    snapshot.resource = swapchain->get_back_buffer(index);
    if (snapshot.resource.handle != 0)
        snapshot.desc = device->get_resource_desc(snapshot.resource);
    return snapshot;
}

static void LogAddonBackBuffer(const AddonBackBufferSnapshot& snapshot, unsigned long long durationUs,
                               bool timingValid)
{
    if (snapshot.resource.handle == 0)
    {
        LogAddonEvent("event=back_buffer index=%u error=resource_unavailable timing_valid=%u probe_us=%llu",
                      snapshot.index, timingValid ? 1u : 0u, durationUs);
        return;
    }

    if (snapshot.desc.type != reshade::api::resource_type::texture_2d)
    {
        LogAddonEvent(
            "event=back_buffer index=%u resource=0x%llX type=%u error=unexpected_resource_type timing_valid=%u "
            "probe_us=%llu",
            snapshot.index, static_cast<unsigned long long>(snapshot.resource.handle),
            static_cast<unsigned int>(snapshot.desc.type), timingValid ? 1u : 0u, durationUs);
        return;
    }

    if (snapshot.desc.texture.width == 0 || snapshot.desc.texture.height == 0)
    {
        LogAddonEvent(
            "event=back_buffer index=%u resource=0x%llX error=invalid_dimensions width=%u height=%u "
            "timing_valid=%u probe_us=%llu",
            snapshot.index, static_cast<unsigned long long>(snapshot.resource.handle), snapshot.desc.texture.width,
            snapshot.desc.texture.height, timingValid ? 1u : 0u, durationUs);
        return;
    }

    LogAddonEvent(
        "event=back_buffer index=%u resource=0x%llX type=%u size=%ux%u format=%u samples=%u usage=0x%08X "
        "flags=0x%08X timing_valid=%u probe_us=%llu",
        snapshot.index, static_cast<unsigned long long>(snapshot.resource.handle),
        static_cast<unsigned int>(snapshot.desc.type), snapshot.desc.texture.width, snapshot.desc.texture.height,
        static_cast<unsigned int>(snapshot.desc.texture.format),
        static_cast<unsigned int>(snapshot.desc.texture.samples), static_cast<unsigned int>(snapshot.desc.usage),
        static_cast<unsigned int>(snapshot.desc.flags), timingValid ? 1u : 0u, durationUs);
}

static void OnAddonInitDevice(reshade::api::device* device)
{
    if (device == nullptr)
    {
        LogAddonEvent("event=init_device error=null_device");
        return;
    }

    LARGE_INTEGER start {};
    const bool timingStarted = StartAddonTiming(start);
    const auto api = device->get_api();
    LUID adapterLuid {};
    const bool hasAdapterLuid = api == reshade::api::device_api::d3d11 &&
                                device->get_property(reshade::api::device_properties::adapter_luid, &adapterLuid);

    unsigned long long durationUs = 0;
    const bool timingValid = timingStarted && GetAddonDurationMicroseconds(start, durationUs);
    if (hasAdapterLuid)
    {
        LogAddonEvent(
            "event=init_device device=%p api=%s api_value=0x%X native=0x%llX adapter_luid=%08X:%08X "
            "timing_valid=%u probe_us=%llu",
            static_cast<void*>(device), GetAddonApiName(api), static_cast<unsigned int>(api),
            static_cast<unsigned long long>(device->get_native()),
            static_cast<unsigned int>(adapterLuid.HighPart), static_cast<unsigned int>(adapterLuid.LowPart),
            timingValid ? 1u : 0u, durationUs);
    }
    else
    {
        LogAddonEvent(
            "event=init_device device=%p api=%s api_value=0x%X native=0x%llX adapter_luid=unavailable "
            "timing_valid=%u probe_us=%llu",
            static_cast<void*>(device), GetAddonApiName(api), static_cast<unsigned int>(api),
            static_cast<unsigned long long>(device->get_native()), timingValid ? 1u : 0u, durationUs);
    }
}

static void OnAddonDestroyDevice(reshade::api::device* device)
{
    if (_addonResourceLifetimes)
    {
        AcquireSRWLockExclusive(&_addonResourceLock);
        size_t remaining = 0;
        const uintptr_t key = reinterpret_cast<uintptr_t>(device);
        for (auto it = _addonResourceGenerations.begin(); it != _addonResourceGenerations.end();)
        {
            if (it->first.first == key)
            {
                ++remaining;
                it = _addonResourceGenerations.erase(it);
            }
            else
                ++it;
        }
        LogAddonEvent("event=resource_lifetime_device_end sequence=%llu device=%p remaining=%llu",
                      static_cast<unsigned long long>(++_addonResourceEventSequence), static_cast<void*>(device),
                      static_cast<unsigned long long>(remaining));
        ReleaseSRWLockExclusive(&_addonResourceLock);
    }
    LogAddonEvent("event=destroy_device device=%p", static_cast<void*>(device));
}

static void OnAddonInitCommandQueue(reshade::api::command_queue* queue)
{
    LogAddonEvent("event=init_command_queue queue=%p", static_cast<void*>(queue));
}

static void OnAddonDestroyCommandQueue(reshade::api::command_queue* queue)
{
    LogAddonEvent("event=destroy_command_queue queue=%p", static_cast<void*>(queue));
}

static void OnAddonInitResource(reshade::api::device* device, const reshade::api::resource_desc& desc,
                                const reshade::api::subresource_data*, reshade::api::resource_usage initialState,
                                reshade::api::resource resource)
{
    if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11 ||
        desc.type != reshade::api::resource_type::texture_2d)
        return;

    if (_addonResourceLifetimes)
    {
        AcquireSRWLockExclusive(&_addonResourceLock);
        const AddonResourceKey key { reinterpret_cast<uintptr_t>(device), resource.handle };
        const auto existing = _addonResourceGenerations.find(key);
        if (existing != _addonResourceGenerations.end())
            LogAddonEvent("event=resource_lifetime_error error=duplicate_live_init device=%p resource=0x%llX generation=%llu",
                          static_cast<void*>(device), static_cast<unsigned long long>(resource.handle),
                          static_cast<unsigned long long>(existing->second));
        else
        {
            const uint64_t generation = ++_addonNextResourceGeneration;
            _addonResourceGenerations.emplace(key, generation);
            LARGE_INTEGER qpc {};
            const bool valid = StartAddonTiming(qpc);
            LogAddonDebug("event=resource_lifetime action=create sequence=%llu device=%p resource=0x%llX generation=%llu qpc_valid=%u qpc=%lld size=%ux%u format=%u",
                          static_cast<unsigned long long>(++_addonResourceEventSequence), static_cast<void*>(device),
                          static_cast<unsigned long long>(resource.handle), static_cast<unsigned long long>(generation),
                          valid ? 1u : 0u, valid ? qpc.QuadPart : 0LL,
                          desc.texture.width, desc.texture.height, static_cast<unsigned>(desc.texture.format));
        }
        ReleaseSRWLockExclusive(&_addonResourceLock);
    }
    LogAddonDebug(
        "event=init_resource device=%p resource=0x%llX type=%u size=%ux%u layers=%u levels=%u format=%u "
        "samples=%u heap=%u usage=0x%08X flags=0x%08X initial_state=0x%08X",
        static_cast<void*>(device), static_cast<unsigned long long>(resource.handle), static_cast<unsigned>(desc.type),
        desc.texture.width, desc.texture.height, static_cast<unsigned>(desc.texture.depth_or_layers),
        static_cast<unsigned>(desc.texture.levels), static_cast<unsigned>(desc.texture.format),
        static_cast<unsigned>(desc.texture.samples), static_cast<unsigned>(desc.heap), static_cast<unsigned>(desc.usage),
        static_cast<unsigned>(desc.flags), static_cast<unsigned>(initialState));
}

static void OnAddonDestroyResource(reshade::api::device* device, reshade::api::resource resource)
{
    if (device != nullptr && device->get_api() == reshade::api::device_api::d3d11)
    {
        if (_addonResourceLifetimes)
        {
            AcquireSRWLockExclusive(&_addonResourceLock);
            const AddonResourceKey key { reinterpret_cast<uintptr_t>(device), resource.handle };
            const auto existing = _addonResourceGenerations.find(key);
            if (existing != _addonResourceGenerations.end())
            {
                LARGE_INTEGER qpc {};
                const bool valid = StartAddonTiming(qpc);
                LogAddonDebug("event=resource_lifetime action=destroy sequence=%llu device=%p resource=0x%llX generation=%llu qpc_valid=%u qpc=%lld",
                              static_cast<unsigned long long>(++_addonResourceEventSequence), static_cast<void*>(device),
                              static_cast<unsigned long long>(resource.handle), static_cast<unsigned long long>(existing->second),
                              valid ? 1u : 0u, valid ? qpc.QuadPart : 0LL);
                _addonResourceGenerations.erase(existing);
            }
            ReleaseSRWLockExclusive(&_addonResourceLock);
        }
        LogAddonDebug("event=destroy_resource device=%p resource=0x%llX", static_cast<void*>(device),
                      static_cast<unsigned long long>(resource.handle));
    }
}

static void OnAddonInitResourceView(reshade::api::device* device, reshade::api::resource resource,
                                    reshade::api::resource_usage usage, const reshade::api::resource_view_desc& desc,
                                    reshade::api::resource_view view)
{
    if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11 ||
        desc.type == reshade::api::resource_view_type::buffer ||
        desc.type == reshade::api::resource_view_type::unknown)
        return;

    LogAddonDebug(
        "event=init_resource_view device=%p resource=0x%llX view=0x%llX usage=0x%08X type=%u format=%u "
        "first_level=%u levels=%u first_layer=%u layers=%u",
        static_cast<void*>(device), static_cast<unsigned long long>(resource.handle),
        static_cast<unsigned long long>(view.handle), static_cast<unsigned>(usage), static_cast<unsigned>(desc.type),
        static_cast<unsigned>(desc.format), desc.texture.first_level, desc.texture.levels, desc.texture.first_layer,
        desc.texture.layers);
}

static void OnAddonDestroyResourceView(reshade::api::device* device, reshade::api::resource_view view)
{
    if (device != nullptr && device->get_api() == reshade::api::device_api::d3d11)
        LogAddonDebug("event=destroy_resource_view device=%p view=0x%llX", static_cast<void*>(device),
                      static_cast<unsigned long long>(view.handle));
}

static void LogAddonBoundView(reshade::api::device* device, reshade::api::command_list* commands,
                              reshade::api::resource_view view, uint64_t sequence, uint64_t presentCounter,
                              const char* role, uint32_t index)
{
    const auto snapshot = InspectAddonView(device, view);
    if (snapshot.status == AddonViewSnapshotStatus::null_view)
        return;
    if (snapshot.status == AddonViewSnapshotStatus::no_resource)
    {
        LogAddonEvent("event=bound_view error=resource_unavailable sequence=%llu device=%p command_list=%p "
                      "present_counter=%llu role=%s index=%u view=0x%llX",
                      static_cast<unsigned long long>(sequence), static_cast<void*>(device),
                      static_cast<void*>(commands), static_cast<unsigned long long>(presentCounter), role, index,
                      static_cast<unsigned long long>(view.handle));
        return;
    }
    if (snapshot.status == AddonViewSnapshotStatus::non_texture_2d)
    {
        LogAddonDebug("event=bound_view sequence=%llu device=%p command_list=%p present_counter=%llu "
                      "role=%s index=%u view=0x%llX resource=0x%llX "
                      "status=non_texture_2d resource_type=%u view_type=%u view_format=%u",
                      static_cast<unsigned long long>(sequence), static_cast<void*>(device),
                      static_cast<void*>(commands), static_cast<unsigned long long>(presentCounter), role, index,
                      static_cast<unsigned long long>(view.handle),
                      static_cast<unsigned long long>(snapshot.resource.handle),
                      static_cast<unsigned>(snapshot.resource_desc.type), static_cast<unsigned>(snapshot.view_desc.type),
                      static_cast<unsigned>(snapshot.view_desc.format));
        return;
    }
    const auto& desc = snapshot.resource_desc;
    const auto& viewDesc = snapshot.view_desc;
    const bool textureView = viewDesc.type >= reshade::api::resource_view_type::texture_1d &&
                             viewDesc.type <= reshade::api::resource_view_type::texture_cube_array;
    if (!textureView)
    {
        LogAddonEvent("event=bound_view error=unexpected_view_type sequence=%llu device=%p command_list=%p "
                      "present_counter=%llu role=%s index=%u view=0x%llX type=%u",
                      static_cast<unsigned long long>(sequence), static_cast<void*>(device),
                      static_cast<void*>(commands), static_cast<unsigned long long>(presentCounter), role, index,
                      static_cast<unsigned long long>(view.handle), static_cast<unsigned>(viewDesc.type));
        return;
    }
    if (_addonResourceLifetimes)
    {
        AcquireSRWLockExclusive(&_addonResourceLock);
        const AddonResourceKey key { reinterpret_cast<uintptr_t>(device), snapshot.resource.handle };
        const auto existing = _addonResourceGenerations.find(key);
        LogAddonDebug("event=resource_lifetime action=observe sequence=%llu bind_sequence=%llu device=%p resource=0x%llX generation=%llu status=%s",
                      static_cast<unsigned long long>(++_addonResourceEventSequence),
                      static_cast<unsigned long long>(sequence), static_cast<void*>(device),
                      static_cast<unsigned long long>(snapshot.resource.handle),
                      static_cast<unsigned long long>(existing == _addonResourceGenerations.end() ? 0 : existing->second),
                      existing == _addonResourceGenerations.end() ? "unknown" : "live");
        ReleaseSRWLockExclusive(&_addonResourceLock);
    }
    LogAddonDebug(
        "event=bound_view sequence=%llu device=%p command_list=%p present_counter=%llu "
        "role=%s index=%u view=0x%llX resource=0x%llX "
        "status=texture_2d size=%ux%u resource_format=%u view_format=%u samples=%u layers=%u levels=%u "
        "view_type=%u first_level=%u view_levels=%u first_layer=%u view_layers=%u usage=0x%08X flags=0x%08X",
        static_cast<unsigned long long>(sequence), static_cast<void*>(device), static_cast<void*>(commands),
        static_cast<unsigned long long>(presentCounter), role, index, static_cast<unsigned long long>(view.handle),
        static_cast<unsigned long long>(snapshot.resource.handle), desc.texture.width, desc.texture.height,
        static_cast<unsigned>(desc.texture.format), static_cast<unsigned>(viewDesc.format),
        static_cast<unsigned>(desc.texture.samples), static_cast<unsigned>(desc.texture.depth_or_layers),
        static_cast<unsigned>(desc.texture.levels), static_cast<unsigned>(viewDesc.type), viewDesc.texture.first_level,
        viewDesc.texture.levels, viewDesc.texture.first_layer, viewDesc.texture.layers,
        static_cast<unsigned>(desc.usage), static_cast<unsigned>(desc.flags));
}

static void OnAddonBindTargets(reshade::api::command_list* commands, uint32_t count,
                               const reshade::api::resource_view* targets, reshade::api::resource_view depth)
{
    if (_addonLogLevel > 1 || commands == nullptr)
        return;
    auto* device = commands->get_device();
    if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11)
        return;
    const uint64_t sequence = _addonBindCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t presentCounter = _addonPresentCount.load(std::memory_order_relaxed);
    const bool diagnosticWindow = _addonAllTargetBindings || InAddonDetailWindow(presentCounter);
    if (!diagnosticWindow && sequence > 30 && sequence % 300 != 0)
        return;
    const bool detailed = diagnosticWindow || sequence <= 30 || sequence % 3000 == 0;
    LARGE_INTEGER start {};
    const bool timingStarted = detailed && StartAddonTiming(start);
    LogAddonDebug("event=bind_targets sequence=%llu command_list=%p device=%p target_count=%u depth_view=0x%llX "
                  "present_counter=%llu detail=%u",
                  static_cast<unsigned long long>(sequence), static_cast<void*>(commands), static_cast<void*>(device),
                  count, static_cast<unsigned long long>(depth.handle),
                  static_cast<unsigned long long>(presentCounter),
                  detailed ? 1u : 0u);
    if (diagnosticWindow)
        LogAddonDebug("event=diagnostic_bind_time sequence=%llu present_counter=%llu qpc_valid=%u qpc=%lld",
                      static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(presentCounter),
                      timingStarted ? 1u : 0u, timingStarted ? start.QuadPart : 0LL);
    if (count != 0 && targets == nullptr)
    {
        LogAddonEvent("event=bind_targets error=null_targets count=%u", count);
        return;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        LogAddonDebug("event=bind_target sequence=%llu index=%u view=0x%llX",
                      static_cast<unsigned long long>(sequence), i, static_cast<unsigned long long>(targets[i].handle));
        if (detailed)
            LogAddonBoundView(device, commands, targets[i], sequence, presentCounter, "color", i);
    }
    if (detailed)
    {
        LogAddonBoundView(device, commands, depth, sequence, presentCounter, "depth", 0);
        unsigned long long durationUs = 0;
        const bool timingValid = timingStarted && GetAddonDurationMicroseconds(start, durationUs);
        LogAddonDebug("event=bound_view_sample sequence=%llu timing_valid=%u probe_us=%llu",
                      static_cast<unsigned long long>(sequence), timingValid ? 1u : 0u, durationUs);
    }
}

static void OnAddonPushDescriptors(reshade::api::command_list* commands, reshade::api::shader_stage stages,
                                   reshade::api::pipeline_layout layout, uint32_t layoutParam,
                                   const reshade::api::descriptor_table_update& update)
{
    using reshade::api::descriptor_type;
    if (commands == nullptr)
        return;
    auto* device = commands->get_device();
    if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11)
        return;
    if (update.type == descriptor_type::sampler || update.type == descriptor_type::constant_buffer)
    {
        const uint64_t sequence = _addonAuxDescriptorCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (update.count && update.descriptors == nullptr)
        {
            LogAddonEvent("event=shader_aux_bind error=null_descriptors sequence=%llu count=%u",
                          static_cast<unsigned long long>(sequence), update.count);
            return;
        }
        LARGE_INTEGER qpc {};
        const bool valid = StartAddonTiming(qpc);
        LogAddonDebug("event=shader_aux_bind sequence=%llu command_list=%p stages=%u layout=0x%llX param=%u "
                      "binding=%u array_offset=%u type=%u count=%u qpc_valid=%u qpc=%lld",
                      static_cast<unsigned long long>(sequence), static_cast<void*>(commands),
                      static_cast<unsigned>(stages), static_cast<unsigned long long>(layout.handle), layoutParam,
                      update.binding, update.array_offset, static_cast<unsigned>(update.type), update.count,
                      valid ? 1u : 0u, valid ? qpc.QuadPart : 0LL);
        for (uint32_t i = 0; i < update.count; ++i)
        {
            if (update.type == descriptor_type::sampler)
            {
                const auto* samplers = static_cast<const reshade::api::sampler*>(update.descriptors);
                LogAddonDebug("event=shader_aux_slot sequence=%llu index=%u binding=%u sampler=0x%llX",
                              static_cast<unsigned long long>(sequence), i, update.binding + i,
                              static_cast<unsigned long long>(samplers[i].handle));
            }
            else
            {
                const auto* buffers = static_cast<const reshade::api::buffer_range*>(update.descriptors);
                LogAddonDebug("event=shader_aux_slot sequence=%llu index=%u binding=%u buffer=0x%llX offset=%llu size=%llu",
                              static_cast<unsigned long long>(sequence), i, update.binding + i,
                              static_cast<unsigned long long>(buffers[i].buffer.handle),
                              static_cast<unsigned long long>(buffers[i].offset),
                              static_cast<unsigned long long>(buffers[i].size));
            }
        }
        return;
    }
    if (update.type != descriptor_type::shader_resource_view &&
        update.type != descriptor_type::unordered_access_view &&
        update.type != descriptor_type::buffer_shader_resource_view &&
        update.type != descriptor_type::buffer_unordered_access_view)
        return;
    const uint64_t sequence = _addonDescriptorCount.fetch_add(1, std::memory_order_relaxed) + 1;
    LARGE_INTEGER qpc {};
    const bool valid = StartAddonTiming(qpc);
    if (update.count && update.descriptors == nullptr)
    {
        LogAddonEvent("event=shader_bind error=null_descriptors sequence=%llu count=%u",
                      static_cast<unsigned long long>(sequence), update.count);
        return;
    }
    LogAddonDebug("event=shader_bind sequence=%llu command_list=%p stages=%u layout=0x%llX param=%u "
                  "binding=%u array_offset=%u type=%u count=%u qpc_valid=%u qpc=%lld",
                  static_cast<unsigned long long>(sequence), static_cast<void*>(commands),
                  static_cast<unsigned>(stages), static_cast<unsigned long long>(layout.handle), layoutParam,
                  update.binding, update.array_offset, static_cast<unsigned>(update.type), update.count,
                  valid ? 1u : 0u, valid ? qpc.QuadPart : 0LL);
    const auto* views = static_cast<const reshade::api::resource_view*>(update.descriptors);
    for (uint32_t i = 0; i < update.count; ++i)
    {
        const auto snapshot = InspectAddonView(device, views[i]);
        if (views[i].handle && snapshot.resource.handle == 0)
            LogAddonEvent("event=shader_view error=resource_unavailable sequence=%llu index=%u",
                          static_cast<unsigned long long>(sequence), i);
        LogAddonDebug("event=shader_view sequence=%llu index=%u binding=%u view=0x%llX resource=0x%llX",
                      static_cast<unsigned long long>(sequence), i, update.binding + i,
                      static_cast<unsigned long long>(views[i].handle),
                      static_cast<unsigned long long>(snapshot.resource.handle));
    }
}

static void InspectAddonNativeSwapchain(uint64_t nativeHandle, uint32_t reshadeCount)
{
    if (nativeHandle == 0)
    {
        LogAddonEvent("event=native_swapchain error=null_native_handle");
        return;
    }

    auto* native = reinterpret_cast<IDXGISwapChain*>(nativeHandle);
    DXGI_SWAP_CHAIN_DESC desc {};
    const HRESULT result = native->GetDesc(&desc);
    if (FAILED(result))
    {
        LogAddonEvent("event=native_swapchain error=get_desc_failed hr=0x%08lX", static_cast<unsigned long>(result));
        return;
    }

    LogAddonEvent(
        "event=native_swapchain native=0x%llX dxgi_buffer_count=%u reshade_buffer_count=%u size=%ux%u "
        "format=%u swap_effect=%u sample_count=%u usage=0x%08X flags=0x%08X windowed=%u",
        static_cast<unsigned long long>(nativeHandle), desc.BufferCount, reshadeCount, desc.BufferDesc.Width,
        desc.BufferDesc.Height, static_cast<unsigned>(desc.BufferDesc.Format), static_cast<unsigned>(desc.SwapEffect),
        desc.SampleDesc.Count, desc.BufferUsage, desc.Flags, desc.Windowed ? 1u : 0u);

    if (desc.BufferCount > 64)
    {
        LogAddonEvent("event=native_swapchain error=unexpected_buffer_count count=%u", desc.BufferCount);
        return;
    }

    for (UINT index = 0; index < desc.BufferCount; ++index)
    {
        ID3D11Texture2D* texture = nullptr;
        const HRESULT hr = native->GetBuffer(index, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        if (FAILED(hr) || texture == nullptr)
        {
            if (index > 0 && hr == DXGI_ERROR_INVALID_CALL)
                LogAddonEvent("event=native_back_buffer index=%u status=not_accessible hr=0x%08lX swap_effect=%u",
                              index, static_cast<unsigned long>(hr), static_cast<unsigned>(desc.SwapEffect));
            else
                LogAddonEvent("event=native_back_buffer index=%u error=get_buffer_failed hr=0x%08lX", index,
                              static_cast<unsigned long>(hr));
            continue;
        }

        D3D11_TEXTURE2D_DESC buffer {};
        texture->GetDesc(&buffer);
        LogAddonEvent(
            "event=native_back_buffer index=%u resource=%p size=%ux%u format=%u samples=%u array_size=%u "
            "mip_levels=%u usage=%u bind_flags=0x%08X cpu_access=0x%08X misc_flags=0x%08X",
            index, static_cast<void*>(texture), buffer.Width, buffer.Height, static_cast<unsigned>(buffer.Format),
            buffer.SampleDesc.Count, buffer.ArraySize, buffer.MipLevels, static_cast<unsigned>(buffer.Usage),
            buffer.BindFlags, buffer.CPUAccessFlags, buffer.MiscFlags);
        texture->Release();
    }
}

static void OnAddonInitSwapchain(reshade::api::swapchain* swapchain, bool resize)
{
    if (swapchain == nullptr)
    {
        LogAddonEvent("event=init_swapchain error=null_swapchain resize=%u", resize ? 1u : 0u);
        return;
    }

    auto* device = swapchain->get_device();
    if (device == nullptr)
    {
        LogAddonEvent("event=init_swapchain swapchain=%p error=null_device resize=%u",
                      static_cast<void*>(swapchain), resize ? 1u : 0u);
        return;
    }

    const auto api = device->get_api();
    if (api != reshade::api::device_api::d3d11)
    {
        LogAddonEvent("event=init_swapchain_ignored swapchain=%p api=%s api_value=0x%X resize=%u",
                      static_cast<void*>(swapchain), GetAddonApiName(api), static_cast<unsigned int>(api),
                      resize ? 1u : 0u);
        return;
    }

    LARGE_INTEGER start {};
    const bool timingStarted = StartAddonTiming(start);
    const uint32_t backBufferCount = swapchain->get_back_buffer_count();
    const uint32_t currentBackBuffer = swapchain->get_current_back_buffer_index();
    const uint64_t nativeSwapchain = swapchain->get_native();
    void* hwnd = swapchain->get_hwnd();
    const auto colorSpace = swapchain->get_color_space();
    unsigned long long durationUs = 0;
    const bool timingValid = timingStarted && GetAddonDurationMicroseconds(start, durationUs);

    LogAddonEvent(
        "event=init_swapchain swapchain=%p native=0x%llX device=%p api=d3d11 hwnd=%p resize=%u "
        "back_buffer_count=%u current_back_buffer=%u color_space=%u timing_valid=%u probe_us=%llu",
        static_cast<void*>(swapchain), static_cast<unsigned long long>(nativeSwapchain), static_cast<void*>(device),
        hwnd, resize ? 1u : 0u, backBufferCount, currentBackBuffer, static_cast<unsigned int>(colorSpace),
        timingValid ? 1u : 0u, durationUs);

    InspectAddonNativeSwapchain(nativeSwapchain, backBufferCount);

    if (backBufferCount == 0)
    {
        LogAddonEvent("event=init_swapchain error=no_back_buffers swapchain=%p",
                      static_cast<void*>(swapchain));
        return;
    }

    if (currentBackBuffer >= backBufferCount)
        LogAddonEvent("event=init_swapchain error=current_back_buffer_out_of_range index=%u count=%u",
                      currentBackBuffer, backBufferCount);

    for (uint32_t index = 0; index < backBufferCount; ++index)
    {
        LARGE_INTEGER bufferStart {};
        const bool bufferTimingStarted = StartAddonTiming(bufferStart);
        const AddonBackBufferSnapshot snapshot = InspectAddonBackBuffer(device, swapchain, index);
        unsigned long long bufferDurationUs = 0;
        const bool bufferTimingValid =
            bufferTimingStarted && GetAddonDurationMicroseconds(bufferStart, bufferDurationUs);
        LogAddonBackBuffer(snapshot, bufferDurationUs, bufferTimingValid);
    }
}

static void OnAddonDestroySwapchain(reshade::api::swapchain* swapchain, bool resize)
{
    LogAddonEvent("event=destroy_swapchain swapchain=%p resize=%u", static_cast<void*>(swapchain), resize ? 1u : 0u);
}

static void OnAddonPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain,
                           const reshade::api::rect*, const reshade::api::rect*, uint32_t,
                           const reshade::api::rect*)
{
    if (swapchain == nullptr)
    {
        LogAddonEvent("event=present error=null_swapchain");
        return;
    }

    auto* device = swapchain->get_device();
    if (device == nullptr)
    {
        LogAddonEvent("event=present error=null_device swapchain=%p", static_cast<void*>(swapchain));
        return;
    }

    if (device->get_api() != reshade::api::device_api::d3d11)
        return;

    const uint64_t frame = _addonPresentCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (InAddonDetailWindow(frame) ||
        (_addonDetailPresentStart != 0 && frame == _addonDetailPresentStart + AddonDetailPresentCount))
    {
        LARGE_INTEGER qpc {};
        const bool valid = StartAddonTiming(qpc);
        LogAddonDebug("event=diagnostic_present present_counter=%llu swapchain=%p qpc_valid=%u qpc=%lld",
                      static_cast<unsigned long long>(frame), static_cast<void*>(swapchain), valid ? 1u : 0u,
                      valid ? qpc.QuadPart : 0LL);
    }
    if (frame > 10 && frame % 300 != 0)
        return;

    if (frame % 300 == 0)
        LogAddonEvent("event=logger_progress present_counter=%llu dropped=%llu dropped_debug=%llu "
                      "dropped_info_or_error=%llu queue_waits=%llu queue_high_water=%llu truncated=%llu",
                      static_cast<unsigned long long>(frame),
                      static_cast<unsigned long long>(_addonDroppedLogs.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(_addonDroppedDebugLogs.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(_addonDroppedImportantLogs.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(_addonLogQueueWaits.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(_addonLogHighWater.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(_addonLogTruncated.load(std::memory_order_relaxed)));

    LARGE_INTEGER start {};
    const bool timingStarted = StartAddonTiming(start);
    const uint32_t backBufferCount = swapchain->get_back_buffer_count();
    const uint32_t currentBackBuffer = swapchain->get_current_back_buffer_index();
    AddonBackBufferSnapshot snapshot {};
    const bool currentBufferInRange = currentBackBuffer < backBufferCount;
    if (currentBufferInRange)
        snapshot = InspectAddonBackBuffer(device, swapchain, currentBackBuffer);

    unsigned long long durationUs = 0;
    const bool timingValid = timingStarted && GetAddonDurationMicroseconds(start, durationUs);
    LogAddonEvent(
        "event=present sequence=%llu api=d3d11 queue=%p swapchain=%p back_buffer_count=%u "
        "current_back_buffer=%u timing_valid=%u probe_us=%llu",
        static_cast<unsigned long long>(frame), static_cast<void*>(queue), static_cast<void*>(swapchain),
        backBufferCount, currentBackBuffer, timingValid ? 1u : 0u, durationUs);

    if (!currentBufferInRange)
    {
        LogAddonEvent("event=present error=current_back_buffer_out_of_range sequence=%llu index=%u count=%u",
                      static_cast<unsigned long long>(frame), currentBackBuffer, backBufferCount);
        return;
    }

    LogAddonBackBuffer(snapshot, durationUs, timingValid);
}
#endif

static std::vector<HMODULE> _asiHandles;
static std::vector<std::filesystem::directory_entry> _lateLoadingEntries;
static bool _passThruMode = false;
static bool _staticLoad = false;

typedef int(WINAPI* PFN_ExeEntry)(PVOID);
static PFN_ExeEntry o_ExeEntry = nullptr;

typedef const char*(CDECL* PFN_wine_get_version)(void);
typedef void (*PFN_InitializeASI)(void);
typedef bool (*PFN_PatchResult)(void);

static inline void* ManualGetProcAddress(HMODULE hModule, const char* functionName)
{
    if (!hModule)
        return nullptr;

    // Verify the alignment
    auto dosHeader = (IMAGE_DOS_HEADER*) hModule;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    auto ntHeaders = (IMAGE_NT_HEADERS*) ((BYTE*) hModule + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    // Look at the export directory
    IMAGE_DATA_DIRECTORY exportData = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exportData.VirtualAddress)
        return nullptr;

    auto exportDir = (IMAGE_EXPORT_DIRECTORY*) ((BYTE*) hModule + exportData.VirtualAddress);

    DWORD* nameRvas = (DWORD*) ((BYTE*) hModule + exportDir->AddressOfNames);
    WORD* ordinalTable = (WORD*) ((BYTE*) hModule + exportDir->AddressOfNameOrdinals);
    DWORD* functionTable = (DWORD*) ((BYTE*) hModule + exportDir->AddressOfFunctions);

    // Iterate over exported names
    for (DWORD i = 0; i < exportDir->NumberOfNames; ++i)
    {
        const char* name = (const char*) hModule + nameRvas[i];
        if (_stricmp(name, functionName) == 0)
        {
            WORD ordinal = ordinalTable[i];
            DWORD funcRva = functionTable[ordinal];
            return (BYTE*) hModule + funcRva;
        }
    }

    return nullptr; // Not found
}

static bool IsRunningOnWine()
{
    LOG_FUNC();

    HMODULE ntdll = GetModuleHandle(L"ntdll.dll");

    if (!ntdll)
    {
        LOG_WARN("Not running on NT!?!");
        return true;
    }

    auto pWineGetVersion = (PFN_wine_get_version) KernelBaseProxy::GetProcAddress_()(ntdll, "wine_get_version");

    // Workaround for the ntdll-Hide_Wine_Exports patch
    if (!pWineGetVersion && KernelBaseProxy::GetProcAddress_()(ntdll, "wine_server_call") != nullptr)
        pWineGetVersion = (PFN_wine_get_version) ManualGetProcAddress(ntdll, "wine_get_version");

    if (pWineGetVersion)
    {
        LOG_INFO("Running on Wine {0}!", pWineGetVersion());
        return true;
    }

    LOG_WARN("Wine not detected");
    return false;
}

uint32_t CheckAgilitySDK(HMODULE exe)
{
    auto version = reinterpret_cast<const uint32_t*>(GetProcAddress(exe, "D3D12SDKVersion"));
    auto path = reinterpret_cast<const char* const*>(GetProcAddress(exe, "D3D12SDKPath"));

    LOG_INFO(" ");

    if (version && path)
    {
        LOG_INFO("Agility SDK Version: {}", *version);
        LOG_INFO("Agility SDK Path: {}", *path);

        return *version;
    }

    LOG_INFO("Agility SDK exports missing");
    return 0;
}

UINT customD3D12SDKVersion = 615;

static void RunAgilityUpgrade(HMODULE dx12Module)
{
    typedef HRESULT (*PFN_IsDeveloperModeEnabled)(BOOL* isEnabled);
    PFN_IsDeveloperModeEnabled o_IsDeveloperModeEnabled =
        (PFN_IsDeveloperModeEnabled) GetProcAddress(GetModuleHandle(L"kernelbase.dll"), "IsDeveloperModeEnabled");

    if (o_IsDeveloperModeEnabled == nullptr)
    {
        LOG_ERROR("Failed to get IsDeveloperModeEnabled function address");
        return;
    }

    auto hk_IsDeveloperModeEnabled = [](BOOL* isEnabled) -> HRESULT
    {
        *isEnabled = TRUE;
        return S_OK;
    };

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_IsDeveloperModeEnabled, static_cast<HRESULT (*)(BOOL*)>(hk_IsDeveloperModeEnabled));
    auto detourResult = DetourTransactionCommit();

    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to attach detour: {:X}", detourResult);
        return;
    }

    if (Config::Instance()->FsrAgilitySDKUpgrade.value_or_default())
    {
        Microsoft::WRL::ComPtr<ID3D12SDKConfiguration> sdkConfig;
        auto hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig));

        if (SUCCEEDED(hr))
        {
            static bool pathSet = false;
            static std::string path; // narrow string

            if (!pathSet)
            {
                std::error_code ec;
                std::wstring widePath = std::filesystem::relative(Config::Instance()->MainDllPath.value(),
                                                                  Util::ExePath().parent_path(), ec);

                // SDK path must be relative to the exe, not possible across drives
                if (!widePath.empty())
                {
                    widePath = widePath + L"\\D3D12_OptiScaler\\";

                    // Properly convert wstring → string
                    int size = WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                    path.resize(size - 1);
                    WideCharToMultiByte(CP_UTF8, 0, widePath.c_str(), -1, path.data(), size, nullptr, nullptr);
                }

                pathSet = true;
            }

            if (path.empty())
            {
                LOG_ERROR("Can't upgrade Agility SDK, Opti folder isn't reachable from the exe folder");
            }
            else
            {
                hr = sdkConfig->SetSDKVersion(customD3D12SDKVersion, reinterpret_cast<LPCSTR>(path.c_str()));
                if (FAILED(hr))
                {
                    LOG_ERROR("Failed to upgrade Agility SDK: {0}", hr);
                }
                else
                {
                    LOG_INFO("Agility SDK upgraded successfully");
                }
            }
        }
        else
        {
            LOG_ERROR("Failed to get D3D12 SDK Configuration interface: {0}", hr);
        }
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&) o_IsDeveloperModeEnabled, static_cast<HRESULT (*)(BOOL*)>(hk_IsDeveloperModeEnabled));
    detourResult = DetourTransactionCommit();

    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to detach detour: {:X}", detourResult);
        return;
    }
}

// Remembers the plugin for the menu
static void RegisterLoadedAsi(const std::filesystem::path& path)
{
    std::string name = wstring_to_string(path.filename().wstring()); // .string() can crash
    std::string version = Util::GetFileVersionString(path.wstring());
    auto& state = State::Instance();
    std::lock_guard<std::mutex> lock(state.loadedAsiPluginsMutex);
    // Late-load entries can be handled more than once
    for (const auto& plugin : state.loadedAsiPlugins)
    {
        if (plugin.name == name)
            return;
    }
    state.loadedAsiPlugins.push_back({ name, version });
}

void LoadAsiPlugins()
{
    std::filesystem::path pluginPath(Config::Instance()->PluginPath.value_or(L"plugins"));
    auto folderPath = pluginPath.wstring();

    LOG_DEBUG(L"Checking {} for *.asi", folderPath);

    if (!std::filesystem::exists(pluginPath))
        return;

    for (const auto& entry : std::filesystem::directory_iterator(folderPath))
    {
        if (!entry.is_regular_file())
            continue;

        std::wstring ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return std::towlower(c); });

        if (ext == L".asi")
        {
            std::wstring fileName = entry.path().filename().wstring();
            std::transform(fileName.begin(), fileName.end(), fileName.begin(),
                           [](wchar_t c) { return std::towlower(c); });

            if (fileName.rfind(L"-loadlate") != std::wstring::npos)
            {
                _lateLoadingEntries.push_back(entry);
                continue;
            }

            HMODULE hMod = NtdllProxy::LoadLibraryExW_Ldr(entry.path().c_str(), NULL, 0);

            if (hMod != nullptr)
            {
                LOG_INFO(L"Loaded: {}", entry.path().wstring());
                _asiHandles.push_back(hMod);

                auto init = (PFN_InitializeASI) KernelBaseProxy::GetProcAddress_()(hMod, "InitializeASI");
                auto patchResult = (PFN_PatchResult) KernelBaseProxy::GetProcAddress_()(hMod, "PatchResult");

                RegisterLoadedAsi(entry.path());

                if (init != nullptr)
                    init();

                PluginApi::InitPlugin(
                    hMod, (PFN_OptiPluginInit) KernelBaseProxy::GetProcAddress_()(hMod, OPTI_PLUGIN_INIT_NAME),
                    entry.path());

                if (patchResult != nullptr)
                {
                    auto pr = patchResult();

                    if (pr)
                    {
                        LOG_INFO("Game patching is successful");
                        State::Instance().isOptiPatcherSucceed = true;

                        LOG_INFO("Disabling spoofing");

                        if (!Config::Instance()->DxgiSpoofing.has_value())
                            Config::Instance()->DxgiSpoofing.set_volatile_value(false);

                        if (!Config::Instance()->VulkanSpoofing.has_value() ||
                            !Config::Instance()->VulkanSpoofing.value_for_config())
                        {
                            Config::Instance()->VulkanSpoofing.set_volatile_value(false);
                        }

                        if (!Config::Instance()->VulkanExtensionSpoofing.has_value())
                            Config::Instance()->VulkanExtensionSpoofing.set_volatile_value(false);
                    }
                }
            }
            else
            {
                DWORD err = GetLastError();
                LOG_ERROR(L"Failed to load: {}, error {:X}", entry.path().wstring(), err);
            }
        }
    }

    if (_lateLoadingEntries.size() > 0)
    {
        std::thread(
            []()
            {
                try
                {

                    auto delay = Config::Instance()->LateAsiPluginsDelay.value_or_default();
                    LOG_INFO("Waiting {} seconds before loading late-load plugins...", delay);
                    std::this_thread::sleep_for(std::chrono::seconds(delay));

                    for (const auto& entry : _lateLoadingEntries)
                    {
                        HMODULE hMod = NtdllProxy::LoadLibraryExW_Ldr(entry.path().c_str(), NULL, 0);

                        if (hMod != nullptr)
                        {
                            LOG_INFO(L"Loaded late: {}", entry.path().wstring());
                            _asiHandles.push_back(hMod);

                            auto init = (PFN_InitializeASI) KernelBaseProxy::GetProcAddress_()(hMod, "InitializeASI");

                            auto patchResult =
                                (PFN_PatchResult) KernelBaseProxy::GetProcAddress_()(hMod, "PatchResult");

                            RegisterLoadedAsi(entry.path());

                            if (init != nullptr)
                                init();

                            PluginApi::InitPlugin(
                                hMod,
                                (PFN_OptiPluginInit) KernelBaseProxy::GetProcAddress_()(hMod, OPTI_PLUGIN_INIT_NAME),
                                entry.path());

                            if (patchResult != nullptr)
                            {
                                auto pr = patchResult();
                                if (pr)
                                {
                                    LOG_INFO("Game patching is successful");
                                    State::Instance().isOptiPatcherSucceed = true;
                                    LOG_INFO("Disabling spoofing");

                                    if (!Config::Instance()->DxgiSpoofing.has_value())
                                        Config::Instance()->DxgiSpoofing.set_volatile_value(false);

                                    if (!Config::Instance()->VulkanSpoofing.has_value() ||
                                        !Config::Instance()->VulkanSpoofing.value_for_config())
                                    {
                                        Config::Instance()->VulkanSpoofing.set_volatile_value(false);
                                    }

                                    if (!Config::Instance()->VulkanExtensionSpoofing.has_value())
                                        Config::Instance()->VulkanExtensionSpoofing.set_volatile_value(false);
                                }
                            }
                        }
                        else
                        {
                            DWORD err = GetLastError();
                            LOG_ERROR(L"Failed to load: {}, error {:X}", entry.path().wstring(), err);
                        }
                    }
                }
                catch (...)
                {
                    LOG_ERROR("Exception occurred while loading late-load plugins");
                }
            })
            .detach();
    }
}

static void CheckWorkingMode()
{
    if (!_passThruMode)
        LOG_FUNC();

    bool modeFound = false;
    std::string filename = wstring_to_string(Util::DllPath().filename().wstring()); // .string() can crash
    std::string lCaseFilename(filename);
    std::filesystem::path pluginPath(Config::Instance()->PluginPath.value_or(L"plugins"));
    auto optiDllPath = std::filesystem::path(Config::Instance()->MainDllPath.value());

    for (size_t i = 0; i < lCaseFilename.size(); i++)
        lCaseFilename[i] = std::tolower(lCaseFilename[i]);

    do
    {
        if (!_passThruMode && Config::Instance()->EarlyHooking.value_or_default())
        {
            NtdllHooks::Hook();
            KernelHooks::Hook();
            KernelHooks::HookBase();
        }

        // version.dll
        if (lCaseFilename == "version.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"version.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as version.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"version-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as version.dll, version-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"version.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as version.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("version.dll");
                dllNames.push_back("version");
                dllNamesW.push_back(L"version.dll");
                dllNamesW.push_back(L"version");

                shared.LoadOriginalLibrary(originalModule);
                version.LoadOriginalLibrary(originalModule);

                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original version.dll!");
            }

            break;
        }

        // winmm.dll
        if (lCaseFilename == "winmm.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"winmm.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as winmm.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"winmm-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as winmm.dll, winmm-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"winmm.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as winmm.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("winmm.dll");
                dllNames.push_back("winmm");
                dllNamesW.push_back(L"winmm.dll");
                dllNamesW.push_back(L"winmm");

                shared.LoadOriginalLibrary(originalModule);
                winmm.LoadOriginalLibrary(originalModule);
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original winmm.dll!");
            }

            break;
        }

        // wininet.dll
        if (lCaseFilename == "wininet.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"wininet.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as wininet.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"wininet-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as wininet.dll, wininet-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"wininet.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as wininet.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("wininet.dll");
                dllNames.push_back("wininet");
                dllNamesW.push_back(L"wininet.dll");
                dllNamesW.push_back(L"wininet");

                shared.LoadOriginalLibrary(originalModule);
                wininet.LoadOriginalLibrary(originalModule);
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original wininet.dll!");
            }

            break;
        }

        // dbghelp.dll
        if (lCaseFilename == "dbghelp.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"dbghelp.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as dbghelp.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"dbghelp-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as dbghelp.dll, dbghelp-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as dbghelp.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("dbghelp.dll");
                dllNames.push_back("dbghelp");
                dllNamesW.push_back(L"dbghelp.dll");
                dllNamesW.push_back(L"dbghelp");

                shared.LoadOriginalLibrary(originalModule);
                dbghelp.LoadOriginalLibrary(originalModule);
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original dbghelp.dll!");
            }

            break;
        }

        // optiscaler.dll
        if (lCaseFilename == "optiscaler.dll")
        {
            if (!_passThruMode)
                LOG_INFO("OptiScaler working as OptiScaler.dll");

            // quick hack for testing
            originalModule = dllModule;

            dllNames.push_back("optiscaler.dll");
            dllNames.push_back("optiscaler");
            dllNamesW.push_back(L"optiscaler.dll");
            dllNamesW.push_back(L"optiscaler");

            modeFound = true;
            break;
        }

        // optiscaler.asi
        if (lCaseFilename == "optiscaler.asi")
        {
            if (!_passThruMode)
                LOG_INFO("OptiScaler working as OptiScaler.asi");

            // quick hack for testing
            originalModule = dllModule;

            dllNames.push_back("optiscaler.asi");
            dllNames.push_back("optiscaler");
            dllNamesW.push_back(L"optiscaler.asi");
            dllNamesW.push_back(L"optiscaler");

            modeFound = true;
            break;
        }

        // winhttp.dll
        if (lCaseFilename == "winhttp.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"winhttp.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as winhttp.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"winhttp-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as winhttp.dll, winhttp-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as winhttp.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("winhttp.dll");
                dllNames.push_back("winhttp");
                dllNamesW.push_back(L"winhttp.dll");
                dllNamesW.push_back(L"winhttp");

                shared.LoadOriginalLibrary(originalModule);
                winhttp.LoadOriginalLibrary(originalModule);
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original winhttp.dll!");
            }

            break;
        }

        // dxgi.dll
        if (lCaseFilename == "dxgi.dll")
        {
            do
            {
                auto pluginFilePath = pluginPath / L"dxgi.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as dxgi.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"dxgi-original.dll", NULL, 0);

                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as dxgi.dll, dxgi-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as dxgi.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("dxgi.dll");
                dllNames.push_back("dxgi");
                dllNamesW.push_back(L"dxgi.dll");
                dllNamesW.push_back(L"dxgi");

                DxgiProxy::Init(originalModule);
                dxgi.LoadOriginalLibrary(originalModule);

                State::Instance().workingMode = WorkingMode::Dxgi;
                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original dxgi.dll!");
            }

            break;
        }

        // d3d12.dll
        if (lCaseFilename == "d3d12.dll")
        {
            do
            {
                // Moved here to cover agility sdk
                if (!_passThruMode)
                {
                    NtdllHooks::Hook();
                    KernelHooks::HookBase();
                }

                auto pluginFilePath = pluginPath / L"d3d12.dll";
                originalModule = NtdllProxy::LoadLibraryExW_Ldr(pluginFilePath.wstring().c_str(), NULL, 0);
                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as d3d12.dll, original dll loaded from plugin folder");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"d3d12-original.dll", NULL, 0);
                if (originalModule != nullptr)
                {
                    if (!_passThruMode)
                        LOG_INFO("OptiScaler working as d3d12.dll, d3d12-original.dll loaded");

                    break;
                }

                originalModule = NtdllProxy::LoadLibraryExW_Ldr(L"d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

                if (originalModule != nullptr && !_passThruMode)
                    LOG_INFO("OptiScaler working as d3d12.dll, system dll loaded");

            } while (false);

            if (originalModule != nullptr)
            {
                dllNames.push_back("d3d12.dll");
                dllNames.push_back("d3d12");
                dllNamesW.push_back(L"d3d12.dll");
                dllNamesW.push_back(L"d3d12");

                D3d12Proxy::Init(originalModule);
                d3d12.LoadOriginalLibrary(originalModule);

                State::Instance().workingMode = WorkingMode::D3d12;

                modeFound = true;
            }
            else
            {
                if (!_passThruMode)
                    LOG_ERROR("OptiScaler can't find original d3d12.dll!");
            }

            break;
        }

    } while (false);

    // Work as a dummy dll
    if (_passThruMode)
        return;

    if (!modeFound)
    {
        LOG_ERROR("Unsupported dll name: {0}", filename);
        return;
    }

    Config::Instance()->CheckUpscalerFiles();

    // Intel Extension Framework
    if (Config::Instance()->UESpoofIntelAtomics64.value_or_default())
    {
        HMODULE igdext = NtdllProxy::LoadLibraryExW_Ldr(L"igdext64.dll", NULL, 0);

        if (igdext == nullptr)
        {
            auto paths = Util::GetDriverStore();

            for (const auto& [luid, path] : paths)
            {
                auto dllPath = path / L"igdext64.dll";
                LOG_DEBUG("Trying to load: {}", wstring_to_string(dllPath.c_str()));
                igdext = NtdllProxy::LoadLibraryExW_Ldr(dllPath.c_str(), NULL, 0);

                if (igdext != nullptr)
                {
                    LOG_INFO(L"igdext64.dll loaded from {}", dllPath.wstring());
                    break;
                }
            }
        }
        else
        {
            LOG_INFO("igdext64.dll loaded from game folder");
        }

        if (igdext != nullptr)
            IGDExtProxy::Init(igdext);
        else
            LOG_ERROR("Failed to load igdext64.dll");
    }

    // DXGI
    if (DxgiProxy::Module() == nullptr)
    {
        LOG_DEBUG("Check for dxgi");
        HMODULE dxgiModule = nullptr;
        dxgiModule = GetDllNameWModule(&dxgiNamesW);
        if (dxgiModule != nullptr)
        {
            LOG_DEBUG("dxgi.dll already in memory");

            DxgiProxy::Init(dxgiModule);
            DxgiHooks::Hook();
        }
    }
    else
    {
        LOG_DEBUG("dxgi.dll already in memory");
        DxgiHooks::Hook();
    }

    // DirectX 12
    if (D3d12Proxy::Module() == nullptr)
    {
        // Moved here to cover agility sdk
        KernelHooks::HookBase();
        NtdllHooks::Hook();

        LOG_DEBUG("Check for d3d12");
        HMODULE d3d12Module = nullptr;
        d3d12Module = GetDllNameWModule(&dx12NamesW);
        if (Config::Instance()->OverlayMenu.value_or_default() && d3d12Module != nullptr)
        {
            LOG_DEBUG("d3d12.dll already in memory");
            D3d12Proxy::Init(d3d12Module);
            D3D12Hooks::Hook();
        }
    }
    else
    {
        LOG_DEBUG("d3d12.dll already in memory");
        D3D12Hooks::Hook();
    }

    if (D3d12Proxy::Module() == nullptr && State::Instance().gameQuirks & GameQuirk::LoadD3D12Manually)
    {
        LOG_DEBUG("Loading d3d12.dll manually");
        D3d12Proxy::Init();
        D3D12Hooks::Hook();
    }

    d3d12AgilityModule = GetDllNameWModule(&dx12agilityNamesW);
    if (d3d12AgilityModule != nullptr)
    {
        LOG_DEBUG("D3D12Core.dll already in memory");
        D3D12Hooks::HookAgility(d3d12AgilityModule);
    }

    if (d3d12AgilityModule == nullptr && State::Instance().gameQuirks & GameQuirk::LoadD3D12Manually)
    {
        auto path = Util::ExePath().parent_path() / L"D3D12" / L"D3D12Core.dll";
        d3d12AgilityModule = NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), NULL, 0);

        if (d3d12AgilityModule == nullptr && Config::Instance()->FsrAgilitySDKUpgrade.value_or_default())
        {
            path = optiDllPath / L"D3D12_OptiScaler" / L"D3D12Core.dll";
            d3d12AgilityModule = NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), NULL, 0);
        }

        if (d3d12AgilityModule == nullptr)
        {
            d3d12AgilityModule = NtdllProxy::LoadLibraryExW_Ldr(L"D3D12Core.dll", NULL, 0);
        }

        if (d3d12AgilityModule != nullptr)
        {
            LOG_DEBUG("D3D12Core.dll loaded");
            D3D12Hooks::HookAgility(d3d12AgilityModule);
        }
    }

    // DirectX 11
    d3d11Module = GetDllNameWModule(&dx11NamesW);
    if (Config::Instance()->OverlayMenu.value_or_default() && d3d11Module != nullptr)
    {
        LOG_DEBUG("d3d11.dll already in memory");
        D3D11Hooks::Hook(d3d11Module);
    }

    // Vulkan
    if (State::Instance().isRunningOnLinux || State::Instance().gameQuirks & GameQuirk::LoadVulkanManually)
    {
        vulkanModule = NtdllProxy::LoadLibraryExW_Ldr(L"vulkan-1.dll", NULL, 0);
        LOG_DEBUG("Loading vulkan-1.dll for Linux, result: {:X}", (size_t) vulkanModule);
    }
    else
    {
        vulkanModule = GetDllNameWModule(&vkNamesW);
    }

    if (vulkanModule != nullptr)
    {
        LOG_DEBUG("Hooking vulkan-1.dll");
        VulkanHooks::Hook(vulkanModule);
    }

    // NVAPI
    // Doesn't seem to like GetModuleHandle for some reason, so call our load to make sure
    if (GetDllNameWModule(&nvapiNamesW) != nullptr)
    {
        // This hooks nvapi as well when possible
        auto nvapi64 = LibraryLoadHooks::LoadNvApi();
    }

    // GDI32
    hookGdi32();

    // Wintrust
    hookWintrust();

    // Crypt32
    hookCrypt32();

    // Advapi32
    if (Config::Instance()->DxgiSpoofing.value_or_default() ||
        Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookAdvapi32();
    }

    // User32
    if (Config::Instance()->SpoofUser32.value_or_default())
    {
        User32Spoofing::Hook();
    }

    // hook streamline right away if it's already loaded
    HMODULE slModule = nullptr;
    slModule = GetDllNameWModule(&slInterposerNamesW);
    if (slModule != nullptr)
    {
        LOG_DEBUG("sl.interposer.dll already in memory");
        StreamlineHooks::hookInterposer(slModule);
        slInterposerModule = slModule;
    }

    HMODULE slDlss = nullptr;
    slDlss = GetDllNameWModule(&slDlssNamesW);
    if (slDlss != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.dlss.dll already in memory");
        StreamlineHooks::hookDlss(slDlss);
    }

    HMODULE slDlssg = nullptr;
    slDlssg = GetDllNameWModule(&slDlssgNamesW);
    if (slDlssg != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.dlss_g.dll already in memory");
        StreamlineHooks::hookDlssg(slDlssg);
    }

    HMODULE slReflex = nullptr;
    slReflex = GetDllNameWModule(&slReflexNamesW);
    if (slReflex != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.reflex.dll already in memory");
        StreamlineHooks::hookReflex(slReflex);
    }

    HMODULE slPcl = nullptr;
    slPcl = GetDllNameWModule(&slPclNamesW);
    if (slPcl != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.pcl.dll already in memory");
        StreamlineHooks::hookPcl(slPcl);
    }

    HMODULE slCommon = nullptr;
    slCommon = GetDllNameWModule(&slCommonNamesW);
    if (slCommon != nullptr)
    {
        State::Instance().postCodes |= PostCode::SlPluginsAlreadyInMemory;
        LOG_WARN("sl.common.dll already in memory");
        StreamlineHooks::hookCommon(slCommon);
    }

    // XeSS
    HMODULE xessModule = nullptr;
    xessModule = GetDllNameWModule(&xessNamesW);
    if (xessModule != nullptr)
    {
        LOG_DEBUG("libxess.dll already in memory");
        XeSSProxy::InitXeSS(xessModule);
    }

    HMODULE xessDx11Module = nullptr;
    xessDx11Module = GetDllNameWModule(&xessDx11NamesW);
    if (xessDx11Module != nullptr)
    {
        LOG_DEBUG("libxess_dx11.dll already in memory");
        XeSSProxy::InitXeSSDx11(xessDx11Module);
    }

    // NVNGX
    HMODULE nvngxModule = nullptr;
    nvngxModule = GetDllNameWModule(&nvngxNamesW);
    if (nvngxModule != nullptr)
    {
        LOG_DEBUG("nvngx.dll already in memory");
        NVNGXProxy::InitNVNGX(nvngxModule);
    }

    // FFX Dx12
    HMODULE ffxDx12Module = nullptr;
    ffxDx12Module = GetDllNameWModule(&ffxDx12NamesW);
    if (ffxDx12Module != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12(ffxDx12Module);
    }

    HMODULE ffxDx12SRModule = nullptr;
    ffxDx12SRModule = GetDllNameWModule(&ffxDx12UpscalerNamesW);
    if (ffxDx12SRModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_upscaler_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12_SR(ffxDx12SRModule);
    }

    HMODULE ffxDx12FGModule = nullptr;
    ffxDx12FGModule = GetDllNameWModule(&ffxDx12FGNamesW);
    if (ffxDx12FGModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_framegeneration_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12_FG(ffxDx12FGModule);
    }

    HMODULE ffxDx12DenoiserModule = nullptr;
    ffxDx12DenoiserModule = GetDllNameWModule(&ffxDx12DenoiserNamesW);
    if (ffxDx12DenoiserModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_denoiser_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12_Denoiser(ffxDx12DenoiserModule);
    }

    HMODULE ffxDx12RadianceModule = nullptr;
    ffxDx12RadianceModule = GetDllNameWModule(&ffxDx12RadianceNamesW);
    if (ffxDx12RadianceModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_radiancecache_dx12.dll already in memory");
        FfxApiProxy::InitFfxDx12_Radiance(ffxDx12RadianceModule);
    }

    // FFX Vulkan
    HMODULE ffxVkModule = nullptr;
    ffxVkModule = GetDllNameWModule(&ffxVkNamesW);
    if (ffxVkModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_vk.dll already in memory");
        FfxApiProxy::InitFfxVk(ffxVkModule);
    }

    HMODULE ffxVkSRModule = nullptr;
    ffxVkSRModule = GetDllNameWModule(&ffxVkUpscalerNamesW);
    if (ffxVkSRModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_upscaler_vk.dll already in memory");
        FfxApiProxy::InitFfxVk_SR(ffxVkSRModule);
    }

    HMODULE ffxVkFGModule = nullptr;
    ffxVkFGModule = GetDllNameWModule(&ffxVkFGNamesW);
    if (ffxVkFGModule != nullptr)
    {
        LOG_DEBUG("amd_fidelityfx_framegeneration_vk.dll already in memory");
        FfxApiProxy::InitFfxVk_FG(ffxVkFGModule);
    }

    // Hook kernel32 methods
    if (!Config::Instance()->EarlyHooking.value_or_default())
    {
        NtdllHooks::Hook();
        KernelHooks::Hook();
    }

    auto gameAgilityVersion = CheckAgilitySDK(exeModule);
    if (State::Instance().isWindows10 && !State::Instance().isRunningOnLinux &&
        !Config::Instance()->FsrAgilitySDKUpgrade.has_value())
    {
        if (gameAgilityVersion < customD3D12SDKVersion)
        {
            LOG_INFO("Enabling AgilitySDKUpgrade");
            Config::Instance()->FsrAgilitySDKUpgrade.set_volatile_value(true);
        }
    }

    // For Agility SDK Upgrade
    if (Config::Instance()->FsrAgilitySDKUpgrade.value_or_default())
    {
        RunAgilityUpgrade(GetDllNameWModule(&dx12NamesW));
    }

    // SpecialK
    if (skModule == nullptr && Config::Instance()->LoadSpecialK.value_or_default())
    {
        auto skFile = Util::ExePath().parent_path() / L"SpecialK64.dll";
        SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");

        State::EnableServeOriginal(200);
        skModule = NtdllProxy::LoadLibraryExW_Ldr(skFile.c_str(), NULL, 0);
        State::DisableServeOriginal(200);

        LOG_INFO("Loading SpecialK64.dll, result: {0:X}", (UINT64) skModule);
    }

    // ReShade
    // Do not load Reshade here is Luma is active and we will create D3D12 device for it
    // We will load Reshade after D3D12 device creation in that case
    if (reshadeModule == nullptr && Config::Instance()->LoadReShade.value_or_default() &&
        !Config::Instance()->CreateD3D12DeviceForLuma.value_or_default())
    {
        auto rsFile = Util::ReShadePath();
        SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK", L"1");

        if (skModule != nullptr)
            SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");

        State::EnableServeOriginal(201);
        reshadeModule = NtdllProxy::LoadLibraryExW_Ldr(rsFile.c_str(), NULL, 0);
        State::DisableServeOriginal(201);

        LOG_INFO("Loading {}, result: {:X}", rsFile.filename().string(), (size_t) reshadeModule);
    }

    // Version check
    if (Config::Instance()->CheckForUpdate.value_or_default())
        VersionCheck::Start();

    return;
}

static void printQuirks(flag_set<GameQuirk>& quirks)
{
    auto state = &State::Instance();
    std::vector<std::string> stringQuirks;

    if (quirks & GameQuirk::CyberpunkHudlessState)
        stringQuirks.push_back("Fixing DLSSG's hudless state in Cyberpunk");

    if (quirks & GameQuirk::FSRFGHudlessMismatchFixup)
        stringQuirks.push_back("FSR FG hudless mismatch fixup");

    if (quirks & GameQuirk::SkipFsr3Method)
        stringQuirks.push_back("Skipping first FSR 3 method");

    if (quirks & GameQuirk::LoadD3D12Manually)
        stringQuirks.push_back("Load d3d12.dll");

    if (quirks & GameQuirk::KernelBaseHooks)
        stringQuirks.push_back("Enable KernelBase hooks");

    if (quirks & GameQuirk::VulkanDLSSBarrierFixup)
        stringQuirks.push_back("Fix DLSS/DLSSG barriers on Vulkan");

    if (quirks & GameQuirk::ForceUnrealEngine)
        stringQuirks.push_back("Force detected engine as Unreal Engine");

    if (quirks & GameQuirk::NoFSRFGFirstSwapchain)
        stringQuirks.push_back("Skip turning the first swapchain created into an FSR swapchain");

    if (quirks & GameQuirk::FixSlSimulationMarkers)
        stringQuirks.push_back("Correct simulation start marker's frame id");

    if (quirks & GameQuirk::HitmanReflexHacks)
        stringQuirks.push_back("Hack for broken Hitman reflex");

    if (quirks & GameQuirk::SkipD3D11FeatureLevelElevation)
        stringQuirks.push_back("Skipping D3D11 feature level elevation, native FSR3.1 will be disabled!");

    if (quirks & GameQuirk::CreateD3D12DeviceForLuma)
        stringQuirks.push_back("Create D3D12 device for Luma before loading Reshade");

    if (quirks & GameQuirk::LoadVulkanManually)
        stringQuirks.push_back("Load vulkan-1.dll");

    if (quirks & GameQuirk::ForceCreateD3D12Device)
        stringQuirks.push_back("Force create D3D12 device for w/Dx12");

    if (quirks & GameQuirk::ForceDepthD32S8)
        stringQuirks.push_back("Force depth as D32S8");

    if (quirks & GameQuirk::PregmataFixDLSSModes)
        stringQuirks.push_back("Fix DLSS quality selection in Pragmata");

    if (quirks & GameQuirk::IgnoreValidUntilEvaluateForFG)
        stringQuirks.push_back("Ignore ValidUntilEvaluate resources for FG");

    if (quirks & GameQuirk::IgnoreTagsWithoutHudlessForFG)
        stringQuirks.push_back("Ignore tagging calls that lack Hudless resource for FG");

    if (quirks & GameQuirk::ForceFGRenderSizeMVs)
        stringQuirks.push_back("Force FG render size motion vectors");

    if (quirks & GameQuirk::CreateSLOnThe2ndDevice)
        stringQuirks.push_back("Create SL on the 2nd device");

    if (quirks & GameQuirk::XeFGCameraMotionFill)
        stringQuirks.push_back("Fill XeFG input motion vectors with camera motion");

    if (quirks & GameQuirk::DLSSGFrameLatencyWaitable)
        stringQuirks.push_back("Request frame latency waitable swapchain for DLSSG");

    if (quirks & GameQuirk::HideFsr4)
        stringQuirks.push_back("Hide FSR 4 from the game's FFX version list");

    if (quirks & GameQuirk::ExtendedSearch)
        stringQuirks.push_back("Extended DLL search");

    state->detectedQuirks.append_range(stringQuirks);
    for (auto& stringQuirk : stringQuirks)
        spdlog::info("Quirk: {}", stringQuirk);

    return;
}

static void CheckQuirks(bool isNvidia)
{
    Util::GetExeInfo();

    LOG_INFO("Game's Exe: {0}", State::Instance().gameExe);
    LOG_INFO("Game Name: {0}", State::Instance().gameName);
    LOG_INFO("Game Version: {0}", State::Instance().gameVersion);
    LOG_INFO("Game Engine: {0}", magic_enum::enum_name(State::Instance().gameEngine));

#ifndef _DEBUG
    // Hash is very slow on Debug builds + we don't need to check our own hashes
    if (Config::Instance()->LogToFile.value_or_default() && Config::Instance()->LogLevel.value_or_default() == 0)
    {
        SHA1 checksum;
        std::ifstream file(Util::ExePath(), std::ios::binary);

        checksum.update(file);
        const std::string hash = checksum.final();

        LOG_TRACE("Game's Exe SHA1: {}", hash);
    }
#endif

    GameConfigContext context;
    context.isNvidia = isNvidia;

    auto quirks = ApplyGameConfigs(State::Instance().gameExe, context);

    // For Luma, we assume if Luma addon in game folder it's used
    const auto dir = Util::ExePath().parent_path();
    bool lumaDetected = false;

    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
            continue;

        const auto& path = entry.path();
        if (path.extension() == L".addon" || path.extension() == L".addon64")
        {
            const auto fname = Util::ToLower(path.filename().wstring());

            // starts with "luma-" or "dlaa-inject"
            if ((fname.rfind(L"luma-", 0) == 0) || (fname.rfind(L"dlaa-inject", 0) == 0))
            {
                lumaDetected = true;
                break;
            }
        }
    }

    if (lumaDetected)
    {
        if (!Config::Instance()->DxgiSpoofing.has_value())
        {
            LOG_INFO("Luma detected, disabling DxgiSpoofing");
            State::Instance().detectedQuirks.push_back("Luma detected, disabling DxgiSpoofing");
            Config::Instance()->DxgiSpoofing.set_volatile_value(false);
        }

        if (!Config::Instance()->DontUseNTShared.has_value())
        {
            LOG_INFO("Luma detected, enabling DontUseNTShared");
            State::Instance().detectedQuirks.push_back("Luma detected, enabling DontUseNTShared");
            Config::Instance()->DontUseNTShared.set_volatile_value(true);
        }

        if (!Config::Instance()->CreateD3D12DeviceForLuma.has_value())
        {
            quirks |= GameQuirk::LoadD3D12Manually;

            if (Config::Instance()->LoadReShade.value_or_default())
            {
                Config::Instance()->CreateD3D12DeviceForLuma.set_volatile_value(true);
                quirks |= GameQuirk::CreateD3D12DeviceForLuma;
            }
        }
    }

    // For Sekiro TSR
    if (std::filesystem::exists(Util::ExePath().parent_path() / L"SekiroTSRLoader.addon"))
    {
        if (!Config::Instance()->DxgiSpoofing.has_value())
        {
            LOG_INFO("Sekiro TSR detected, disabling DxgiSpoofing");
            State::Instance().detectedQuirks.push_back("Luma UE detected, disabling DxgiSpoofing");
            Config::Instance()->DxgiSpoofing.set_volatile_value(false);
        }

        if (!Config::Instance()->DontUseNTShared.has_value())
        {
            LOG_INFO("Sekiro TSR detected, enabling DontUseNTShared");
            State::Instance().detectedQuirks.push_back("Sekiro TSR detected, enabling DontUseNTShared");
            Config::Instance()->DontUseNTShared.set_volatile_value(true);
        }

        if (!Config::Instance()->CreateD3D12DeviceForLuma.has_value())
        {
            quirks |= GameQuirk::LoadD3D12Manually;

            if (Config::Instance()->LoadReShade.value_or_default())
            {
                Config::Instance()->CreateD3D12DeviceForLuma.set_volatile_value(true);
                quirks |= GameQuirk::CreateD3D12DeviceForLuma;
            }
        }
    }

    // if (!Config::Instance()->DxgiFactoryWrapping.has_value() && Config::Instance()->LoadReShade.value_or_default() &&
    //     quirks & GameQuirk::CreateD3D12DeviceForLuma && State::Instance().activeFgInput != FGInput::NoFG &&
    //     State::Instance().activeFgInput != FGInput::NvngxFG)
    //{
    //     Config::Instance()->DxgiFactoryWrapping.set_volatile_value(true);
    //     State::Instance().detectedQuirks.push_back("Factory wrapping enabled due to delayed ReShade + FG");
    //     LOG_INFO("Factory wrapping enabled due to delayed ReShade + FG");
    // }

    if (Config::Instance()->LoadSpecialK.value_or_default() && State::Instance().activeFgInput != FGInput::NoFG &&
        State::Instance().activeFgInput != FGInput::NvngxFG)
    {
        Config::Instance()->LoadSpecialK.set_volatile_value(false);
        State::Instance().detectedQuirks.push_back("FG Inputs are enabled, LoadSpecialK disabled");
        LOG_INFO("FG Inputs are enabled, LoadSpecialK disabled");
    }

    State::Instance().gameQuirks = quirks;

    printQuirks(quirks);
}

void CheckForExcludedProcess()
{
    std::wstring exeLower = Util::ExePath().filename().wstring();
    std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::towlower);

    // If target process is set, only that process is hooked
    if (Config::Instance()->TargetProcess.has_value())
    {
        // Config reads string as lowercase already
        std::wstring targetProcess = Config::Instance()->TargetProcess.value();

        if (exeLower != targetProcess)
        {
            _passThruMode = true;
            return;
        }
    }

    // Config reads string as lowercase already
    static const std::wstring exclusionList = Config::Instance()->ProcessExclusionList.value_or_default() + L"|";

    static std::vector<std::wstring> exclusions = []()
    {
        std::vector<std::wstring> result;
        size_t start = 0, end;

        while ((end = exclusionList.find(L'|', start)) != std::wstring::npos)
        {
            result.emplace_back(exclusionList.substr(start, end - start));
            start = end + 1;
        }
        return result;
    }();

    for (auto& e : exclusions)
    {
        if (exeLower == e)
        {
            _passThruMode = true;
            return;
        }
    }

    _passThruMode = false;
}

void CheckMemoryForProxies()
{
    FfxApiProxy::InitFfxDx12();
    FfxApiProxy::InitFfxDx12_SR();
    FfxApiProxy::InitFfxDx12_FG();
    FfxApiProxy::InitFfxDx12_Denoiser();
    FfxApiProxy::InitFfxDx12_Radiance();

    // Only the game's XeFG already in memory, the one OptiScaler loads next is hooked once the game loads it too
    auto gameXeFG = KernelBaseProxy::GetModuleHandleW_()(L"libxess_fg.dll");

    XeSSProxy::InitXeSS();
    XeSSProxy::InitXeSSDx11();
    XeFGProxy::InitXeFG();
    XeLLProxy::InitXeLL();
    XeFGInputs::Hook(gameXeFG);

    NVNGXProxy::InitNVNGX();
}

DWORD WINAPI getGpuInfo(LPVOID hModuleVoid)
{
    // TODO: dxvk deadlocks when the game and identify gpu calls create at the same time
    // Sleep(1000);

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // We don't yet know if the GPU supports FSR 4 so hook any AMD
    if (primaryGpu.vendorId == VendorId::AMD)
        Amdxc64Hooks::Init();

    else if (IsFsr4Int8(Config::Instance()->Fsr4GpuSupport.value_or_default()))
    {
        // We need spoofing hooks for FFX but want to avoid spoofing for the rest of the game
        if (!Config::Instance()->DxgiSpoofing.value_or_default())
        {
            std::wstring wname = string_to_wstring(primaryGpu.name);
            Config::Instance()->SpoofedVendorId.set_volatile_value(primaryGpu.vendorId);
            Config::Instance()->SpoofedDeviceId.set_volatile_value(primaryGpu.deviceId);
            Config::Instance()->SpoofedGPUName.set_volatile_value(wname);
        }

        Config::Instance()->DxgiSpoofing.set_volatile_value(true);
    }

    // If DX12 already loaded then grab the full GPU info right away
    if (hModuleVoid)
        IdentifyGpu::updateD3d12Capabilities();

    return 0;
}

#if defined(OPTISCALER_RESHADE_ADDON)
extern "C" __declspec(dllexport) bool AddonInit(HMODULE hModule, HMODULE reshadeModule)
{
        ConfigureAddonDetailWindow();
        if (!InitializeAddonLogPath(hModule))
        {
            OutputDebugStringA("OptiScaler ReShade log: failed to resolve the add-on log path\n");
            return false;
        }
        if (!StartAddonLogger())
            return false;
        if (!ConfigureAddonAllTargetBindings())
        {
            LogAddonEvent("event=target_bind_capture error=invalid_configuration log_level=%d file=%u",
                          _addonLogLevel, _addonLogToFile ? 1u : 0u);
            StopAddonLogger();
            return false;
        }
        if (!ConfigureAddonShaderBindings())
        {
            LogAddonEvent("event=shader_bind_capture error=invalid_configuration log_level=%d file=%u",
                          _addonLogLevel, _addonLogToFile ? 1u : 0u);
            StopAddonLogger();
            return false;
        }
        if (!ConfigureAddonResourceLifetimes())
        {
            LogAddonEvent("event=resource_lifetime_capture error=invalid_configuration log_level=%d file=%u",
                          _addonLogLevel, _addonLogToFile ? 1u : 0u);
            StopAddonLogger();
            return false;
        }

        LogAddonEvent("event=addon_attach module=%p", static_cast<void*>(hModule));
        LogAddonEvent(
            "event=log_configuration level=%d file=%u console=%u debug=%u "
            "ngx_callback=unsupported async=internal_bounded_wait dumps=unsupported console_creation=unsupported "
            "resource_scope=observed_dx11_texture_events bind_sampling=%s bound_view_sampling=%s",
            _addonLogLevel, _addonLogToFile ? 1u : 0u, _addonLogToConsole ? 1u : 0u,
            _addonLogToDebug ? 1u : 0u, _addonAllTargetBindings ? "all" : "first_30_then_every_300",
            _addonAllTargetBindings ? "all" : "first_30_then_every_3000");
        LogAddonEvent("event=diagnostic_window detail_start=%llu present_intervals=%llu",
                      static_cast<unsigned long long>(_addonDetailPresentStart),
                      static_cast<unsigned long long>(AddonDetailPresentCount));
        LogAddonEvent("event=target_bind_capture mode=%s scope=dx11_render_targets_and_depth",
                      _addonAllTargetBindings ? "all" : "sampled");
        LogAddonEvent("event=shader_bind_capture mode=%s scope=dx11_push_srv_uav_sampler_cb",
                      _addonShaderBindings ? "all" : "disabled");
        LogAddonEvent("event=resource_lifetime_capture mode=%s scope=observed_dx11_texture2d",
                      _addonResourceLifetimes ? "enabled" : "disabled");
        if (_addonDetailConfigError)
            LogAddonEvent("event=diagnostic_window error=invalid_detail_start");
        if (!reshade::register_addon(hModule, reshadeModule))
        {
            LogAddonEvent("event=addon_registration_failed");
            StopAddonLogger();
            return false;
        }
        _addonRegistered = true;

        reshade::register_event<reshade::addon_event::init_device>(OnAddonInitDevice);
        reshade::register_event<reshade::addon_event::destroy_device>(OnAddonDestroyDevice);
        reshade::register_event<reshade::addon_event::init_command_queue>(OnAddonInitCommandQueue);
        reshade::register_event<reshade::addon_event::destroy_command_queue>(OnAddonDestroyCommandQueue);
        reshade::register_event<reshade::addon_event::init_resource>(OnAddonInitResource);
        reshade::register_event<reshade::addon_event::destroy_resource>(OnAddonDestroyResource);
        reshade::register_event<reshade::addon_event::init_resource_view>(OnAddonInitResourceView);
        reshade::register_event<reshade::addon_event::destroy_resource_view>(OnAddonDestroyResourceView);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnAddonBindTargets);
        if (_addonShaderBindings)
            reshade::register_event<reshade::addon_event::push_descriptors>(OnAddonPushDescriptors);
        reshade::register_event<reshade::addon_event::init_swapchain>(OnAddonInitSwapchain);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(OnAddonDestroySwapchain);
        reshade::register_event<reshade::addon_event::present>(OnAddonPresent);
        LogAddonEvent("event=addon_registered api_version=%u", RESHADE_API_VERSION);
        return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE hModule, HMODULE reshadeModule)
{
        if (_addonRegistered)
        {
            if (_addonShaderBindings)
                reshade::unregister_event<reshade::addon_event::push_descriptors>(OnAddonPushDescriptors);
            reshade::unregister_event<reshade::addon_event::present>(OnAddonPresent);
            reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnAddonDestroySwapchain);
            reshade::unregister_event<reshade::addon_event::init_swapchain>(OnAddonInitSwapchain);
            reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnAddonBindTargets);
            reshade::unregister_event<reshade::addon_event::destroy_resource_view>(OnAddonDestroyResourceView);
            reshade::unregister_event<reshade::addon_event::init_resource_view>(OnAddonInitResourceView);
            reshade::unregister_event<reshade::addon_event::destroy_resource>(OnAddonDestroyResource);
            reshade::unregister_event<reshade::addon_event::init_resource>(OnAddonInitResource);
            reshade::unregister_event<reshade::addon_event::destroy_command_queue>(OnAddonDestroyCommandQueue);
            reshade::unregister_event<reshade::addon_event::init_command_queue>(OnAddonInitCommandQueue);
            reshade::unregister_event<reshade::addon_event::destroy_device>(OnAddonDestroyDevice);
            reshade::unregister_event<reshade::addon_event::init_device>(OnAddonInitDevice);
        }
        LogAddonEvent("event=addon_detach presents=%llu target_bind_callbacks=%llu shader_bind_callbacks=%llu shader_aux_callbacks=%llu",
                      static_cast<unsigned long long>(_addonPresentCount.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(_addonBindCount.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(_addonDescriptorCount.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(_addonAuxDescriptorCount.load(std::memory_order_relaxed)));
        if (_addonRegistered)
            reshade::unregister_addon(hModule, reshadeModule);
        _addonRegistered = false;
        StopAddonLogger();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}
#else
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);

        _staticLoad = lpReserved != nullptr;

        HMODULE handle = nullptr;
        OSVERSIONINFOW winVer { 0 };

        State::Instance().startTime = std::chrono::steady_clock::now();

        dllModule = hModule;
        exeModule = GetModuleHandle(nullptr);
        processId = GetCurrentProcessId();

        if (!Config::Instance()->MainDllPath.has_value())
            Config::Instance()->MainDllPath.set_volatile_value(L"OptiScaler");

        if (std::filesystem::path mainDllPath(Config::Instance()->MainDllPath.value()); mainDllPath.is_relative())
            Config::Instance()->MainDllPath.set_volatile_value(Util::DllPath().parent_path() / mainDllPath);

        if (!std::filesystem::exists(Config::Instance()->MainDllPath.value()) ||
            !std::filesystem::is_directory(Config::Instance()->MainDllPath.value()))
            Config::Instance()->MainDllPath.set_volatile_value(Util::DllPath().parent_path());

        Config::Instance()->MainDllPath.set_volatile_value(
            std::filesystem::absolute(Config::Instance()->MainDllPath.value()));

        if (!Config::Instance()->PluginPath.has_value() ||
            (!std::filesystem::exists(Config::Instance()->PluginPath.value()) ||
             !std::filesystem::is_directory(Config::Instance()->PluginPath.value())))
            Config::Instance()->PluginPath.set_volatile_value(
                std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"plugins");

        CheckForExcludedProcess();
        if (_passThruMode)
        {
            NtdllProxy::Init();
            KernelBaseProxy::Init();
            Kernel32Proxy::Init();
            CheckWorkingMode();
            return true;
        }

        if (!Config::Instance()->LogToFile.has_value())
            Config::Instance()->LogToFile.set_volatile_value(true);

#ifdef _DEBUG
        if (Config::Instance()->LogLevel.value_or_default() > 1)
            Config::Instance()->LogLevel.set_volatile_value(1);
#endif

        PrepareLogger();
        spdlog::info("Proxy log file: {}", wstring_to_string(Config::Instance()->LogFileName.value_or_default()));
        spdlog::warn("{0} loaded", BuildInfo::ProductName());
        spdlog::warn("---------------------------------");
        spdlog::warn("OptiScaler is freely downloadable from");
        spdlog::warn("GitHub : https://github.com/optiscaler/OptiScaler/releases");
        spdlog::warn("Nexus  : https://www.nexusmods.com/site/mods/986");
        spdlog::warn("If you paid for these files, you've been scammed!");
        spdlog::warn("DO NOT USE IN MULTIPLAYER GAMES");
        spdlog::info("");
        spdlog::info("LogLevel: {}", Config::Instance()->LogLevel.value_or_default());

        CustomProviders::ResolveAll();
        spdlog::info("");
        if (Util::GetRealWindowsVersion(winVer))
            spdlog::info("Windows version: {} ({}.{}.{})", Util::GetWindowsName(winVer), winVer.dwMajorVersion,
                         winVer.dwMinorVersion, winVer.dwBuildNumber, winVer.dwPlatformId);
        else
            spdlog::warn("Can't read windows version");

        State::Instance().isWindows10 = winVer.dwMajorVersion == 10 && winVer.dwBuildNumber < 22000;
        spdlog::info("");
        spdlog::info("Config parameters:");
        for (const std::string& line : Config::Instance()->GetConfigLog())
            spdlog::info(line);
        spdlog::info("");
        spdlog::info("Setting DllPath to {}", wstring_to_string(Config::Instance()->MainDllPath.value()));
        spdlog::info("");

#ifdef VER_PRE_RELEASE
        spdlog::info("Pre-release build, disabling update checks");
        Config::Instance()->CheckForUpdate.set_volatile_value(false);
#endif

        State::Instance().activeFgInput = Config::Instance()->FGInput.value_or_default();
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
        State::Instance().activeFgNvngx = Config::Instance()->FGNvngxReplacement.value_or_default();
        State::Instance().activeFgNvngxCustom = State::Instance().fgNvngxCustom;
        State::Instance().activeFsrFgProvider = Config::Instance()->FSRFGProvider.value_or("");

        if (State::Instance().activeFgOutput == FGOutput::NoFG && State::Instance().activeFgInput != FGInput::NoFG &&
            State::Instance().activeFgInput != FGInput::NvngxFG &&
            State::Instance().activeFgInput != FGInput::ForceXeLL)
        {
            spdlog::warn("FG Input {} without an FG Output, using none",
                         magic_enum::enum_name(State::Instance().activeFgInput));
            State::Instance().activeFgInput = FGInput::NoFG;
        }

        if (State::Instance().activeFgInput != FGInput::NvngxFG &&
            State::Instance().activeFgOutput != FGOutput::DLSSG &&
            State::Instance().activeFgOutput != FGOutput::FlipMeter)
            State::Instance().activeFgNvngx = FGNvngxReplacement::None;

        if (State::Instance().activeFgInput == FGInput::NvngxFG)
            State::Instance().activeFgOutput = FGOutput::NoFG;
        if (XeFGInputs::Passthrough())
            State::Instance().activeFgOutput = FGOutput::NoFG;

        NtdllProxy::Init();
        KernelBaseProxy::Init();
        Kernel32Proxy::Init();
        CrashDumps::Init();

        spdlog::info("");
        State::Instance().isRunningOnLinux = IsRunningOnWine();

        auto isNvidiaViaNvapi = [&]()
        {
            bool nvidiaDetected = false;
            auto nvapiModule = NtdllProxy::LoadLibraryExW_Ldr(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!nvapiModule)
            {
                spdlog::debug("Nvidia detected: {}", nvidiaDetected);
                return nvidiaDetected;
            }

            if (auto queryInterface = reinterpret_cast<PFN_NvApi_QueryInterface>(
                    KernelBaseProxy::GetProcAddress_()(nvapiModule, "nvapi_QueryInterface")))
            {
                NvAPI_ShortString description;
                auto* getVersion = GET_INTERFACE(NvAPI_GetInterfaceVersionString, queryInterface);
                if (getVersion && getVersion(description) == NVAPI_OK &&
                    (std::string_view(description) == "NVAPI Open Source Interface (DXVK-NVAPI)" ||
                     std::string_view(description) == "DXVK_NVAPI"))
                {
                    spdlog::debug("Using dxvk-nvapi");
                    DISPLAY_DEVICEA displayDevice = {};
                    displayDevice.cb = sizeof(displayDevice);
                    int deviceIndex = 0;
                    while (EnumDisplayDevicesA(nullptr, deviceIndex, &displayDevice, 0))
                    {
                        if ((displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE) &&
                            std::string_view(displayDevice.DeviceID).contains("VEN_10DE"))
                        {
                            nvidiaDetected = true;
                            break;
                        }
                        ++deviceIndex;
                    }

                    HKEY pciKey = nullptr;
                    if (!nvidiaDetected && State::Instance().isRunningOnLinux &&
                        RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ,
                                      &pciKey) == ERROR_SUCCESS)
                    {
                        wchar_t deviceName[256];
                        for (DWORD i = 0; !nvidiaDetected; ++i)
                        {
                            DWORD deviceNameLength = std::size(deviceName);
                            if (RegEnumKeyExW(pciKey, i, deviceName, &deviceNameLength, nullptr, nullptr, nullptr,
                                              nullptr) != ERROR_SUCCESS)
                                break;
                            if (!std::wstring_view(deviceName).starts_with(L"VEN_10DE"))
                                continue;

                            HKEY deviceKey = nullptr;
                            if (RegOpenKeyExW(pciKey, deviceName, 0, KEY_READ, &deviceKey) != ERROR_SUCCESS)
                                continue;

                            wchar_t instanceName[256];
                            for (DWORD j = 0; !nvidiaDetected; ++j)
                            {
                                DWORD instanceNameLength = std::size(instanceName);
                                if (RegEnumKeyExW(deviceKey, j, instanceName, &instanceNameLength, nullptr, nullptr,
                                                  nullptr, nullptr) != ERROR_SUCCESS)
                                    break;

                                wchar_t classGuid[64];
                                DWORD classGuidSize = sizeof(classGuid);
                                if (RegGetValueW(deviceKey, instanceName, L"ClassGUID", RRF_RT_REG_SZ, nullptr,
                                                 classGuid, &classGuidSize) == ERROR_SUCCESS &&
                                    _wcsicmp(classGuid, L"{4d36e968-e325-11ce-bfc1-08002be10318}") == 0)
                                {
                                    spdlog::debug("Nvidia GPU found: {}", wstring_to_string(deviceName));
                                    nvidiaDetected = true;
                                }
                            }
                            RegCloseKey(deviceKey);
                        }
                        RegCloseKey(pciKey);
                    }
                }
                else if (queryInterface(0x21382138))
                {
                    spdlog::error("Using fakenvapi as nvapi64.dll, remove it!");
                }
                else
                {
                    spdlog::debug("Using Nvidia's nvapi");
                    auto init = GET_INTERFACE(NvAPI_Initialize, queryInterface);
                    if (init && init() == NVAPI_OK)
                    {
                        nvidiaDetected = true;
                        if (auto unload = GET_INTERFACE(NvAPI_Unload, queryInterface))
                            unload();
                    }
                }
            }
            NtdllProxy::FreeLibrary_Ldr(nvapiModule);
            spdlog::debug("Nvidia detected: {}", nvidiaDetected);
            return nvidiaDetected;
        };

        const bool possibleNvidia = isNvidiaViaNvapi();
        IdentifyGpu::setNvidiaHint(possibleNvidia);
        spdlog::info("");
        CheckQuirks(possibleNvidia);
        spdlog::info("");
        spdlog::info("Check for DLSS files");

        auto exePath = Util::ExePath().remove_filename();
        auto optiDllPath = std::filesystem::path(Config::Instance()->MainDllPath.value());
        std::vector<std::filesystem::path> dllFolders;
        if (Config::Instance()->DLSSFeaturePath.has_value())
            dllFolders.push_back(Config::Instance()->DLSSFeaturePath.value());

        auto findDll = [&](const wchar_t* name) -> std::optional<std::filesystem::path>
        {
            for (const auto& folder : dllFolders)
            {
                std::error_code ec;
                if (auto path = folder / name; std::filesystem::is_regular_file(path, ec))
                {
                    LOG_INFO(L"{} found at {}", name, folder.wstring());
                    return path;
                }
            }
            return Util::FindFilePath(exePath, name);
        };

        if (Config::Instance()->NVNGX_DLSS_Library.has_value())
        {
            std::filesystem::path dlssPath(Config::Instance()->NVNGX_DLSS_Library.value());
            std::error_code ec;
            if (std::filesystem::is_directory(dlssPath, ec))
                dlssPath /= L"nvngx_dlss.dll";

            if (std::filesystem::is_regular_file(dlssPath, ec))
            {
                Config::Instance()->NVNGX_DLSS_Library.set_volatile_value(dlssPath.wstring());
                State::Instance().NVNGX_DLSS_Path = dlssPath.wstring();
                dllFolders.push_back(dlssPath.parent_path());
                LOG_INFO("nvngx_dlss.dll override found at {}", wstring_to_string(dlssPath.wstring()));
            }
            else
            {
                LOG_WARN("nvngx_dlss.dll override not found at {}, ignoring it", wstring_to_string(dlssPath.wstring()));
                Config::Instance()->NVNGX_DLSS_Library.reset_volatile();
            }
        }

        dllFolders.push_back(optiDllPath);
        if (!State::Instance().NVNGX_DLSS_Path.has_value())
            State::Instance().NVNGX_DLSS_Path = findDll(L"nvngx_dlss.dll");
        State::Instance().NVNGX_DLSSD_Path = findDll(L"nvngx_dlssd.dll");
        State::Instance().NVNGX_DLSSG_Path = findDll(L"nvngx_dlssg.dll");

        if (Config::Instance()->DLSSEnabled.value_or_default() && possibleNvidia)
        {
            if (State::Instance().NVNGX_DLSS_Path.has_value())
            {
                spdlog::info("Enabling DLSS");
                Config::Instance()->DLSSEnabled.set_volatile_value(true);
            }
            else
            {
                spdlog::warn("nvngx_dlss.dll not found, disabling DLSS");
                Config::Instance()->DLSSEnabled.set_volatile_value(false);
            }
            if (!Config::Instance()->DxgiSpoofing.has_value())
            {
                spdlog::info("Disabling DxgiSpoofing");
                Config::Instance()->DxgiSpoofing.set_volatile_value(false);
            }
        }
        else
        {
            Config::Instance()->DLSSEnabled.set_volatile_value(false);
        }

        spdlog::info("");
        CheckWorkingMode();
        CheckMemoryForProxies();
        if (Config::Instance()->FGInput.value_or_default() == FGInput::Upscaler &&
            !Config::Instance()->DisableOverlays.has_value())
            Config::Instance()->DisableOverlays.set_volatile_value(true);
        if (Config::Instance()->DisableOverlays.value_or_default())
        {
            _wputenv_s(L"SteamNoOverlayUIDrawing", L"1");
            SetEnvironmentVariableW(L"SteamNoOverlayUIDrawing", L"1");
        }

        if (Config::Instance()->Fsr4EnableWatermark.has_value())
        {
            Util::SetFlagEnvVar(L"MLSR-WATERMARK", Config::Instance()->Fsr4EnableWatermark.value());
            if (Config::Instance()->Fsr4EnableWatermark.value() &&
                !Config::Instance()->FpsOverlayPosition.has_value())
                Config::Instance()->FpsOverlayPosition.set_volatile_value(FpsOverlayPos_TopRight);
        }
        if (Config::Instance()->FSRFGEnableWatermark.has_value())
        {
            Util::SetFlagEnvVar(L"MLFI-WATERMARK", Config::Instance()->FSRFGEnableWatermark.value());
            if (Config::Instance()->FSRFGEnableWatermark.value() &&
                !Config::Instance()->FpsOverlayPosition.has_value())
                Config::Instance()->FpsOverlayPosition.set_volatile_value(FpsOverlayPos_TopRight);
        }

        if (Config::Instance()->LoadAsiPlugins.value_or_default())
        {
            spdlog::info("");
            LoadAsiPlugins();
        }
        if (!Config::Instance()->DxgiSpoofing.has_value() && !State::Instance().nvngxReplacement.has_value())
        {
            LOG_WARN("Nvngx replacement not found!");
            if (!State::Instance().nvngxExists)
            {
                LOG_WARN("nvngx.dll not found! - disabling spoofing");
                Config::Instance()->DxgiSpoofing.set_volatile_value(false);
            }
        }

        if (Config::Instance()->EnableFsr2Inputs.value_or_default())
        {
            spdlog::info("");
            if (Config::Instance()->UseFsr2VulkanInputs.value_or_default())
                HookFSR2VkExeInputs();
            else if (Config::Instance()->UseFsr2Dx11Inputs.value_or_default())
                HookFSR2Dx11ExeInputs();
            else
            {
                handle = GetDllNameWModule(&fsr2NamesW);
                if (handle != nullptr)
                    HookFSR2Inputs(handle);
                handle = GetDllNameWModule(&fsr2BENamesW);
                if (handle != nullptr)
                    HookFSR2Dx12Inputs(handle);
                HookFSR2ExeInputs();
            }
        }
        if (Config::Instance()->EnableFsr3Inputs.value_or_default())
        {
            handle = GetDllNameWModule(&fsr3NamesW);
            if (handle != nullptr)
                HookFSR3Inputs(handle);
            handle = GetDllNameWModule(&fsr3BENamesW);
            if (handle != nullptr)
                HookFSR3Dx12Inputs(handle);
            HookFSR3ExeInputs();
        }

        if (State::Instance().activeFgInput == FGInput::FSRFG30)
        {
            FSR3FG::HookFSR3FGInputs();
            FSR3FG::HookFSR3FGExeInputs();
        }
        if (State::Instance().activeFgInput == FGInput::Upscaler &&
            State::Instance().gameEngine == GameEngineType::Unity && !Config::Instance()->FGResourceFlip.has_value())
        {
            LOG_WARN("Unity detected with Upscaler input, but FGResourceFlip is not set. Enabling it");
            Config::Instance()->FGResourceFlip.set_volatile_value(true);
        }
        for (size_t i = 0; i < 300; ++i)
        {
            State::Instance().frameTimes.push_back(0.0f);
            State::Instance().upscaleTimes.push_back(0.0f);
        }

        spdlog::info("");
        spdlog::info("Init done");
        spdlog::info("---------------------------------------------");
        spdlog::info("");
        if (o_ExeEntry == nullptr)
            CreateThread(nullptr, 0, getGpuInfo, GetDllNameWModule(&dx12NamesW), 0, nullptr);

#ifndef _DEBUG
        if (Config::Instance()->LogLevel.value_or_default() == 0 && Config::Instance()->LogToFile.value_or_default())
        {
            std::thread(
                []()
                {
                    std::this_thread::sleep_for(std::chrono::minutes(10));
                    if (Config::Instance()->LogLevel.value_or_default() == 0 &&
                        Config::Instance()->LogToFile.value_or_default())
                    {
                        ImGuiToast notification({ ImGuiToastType::Warning, 30000,
                                                  "That's likely unintended and will lead to big OptiScaler.log\n"
                                                  "Please disable logging to file and delete OptiScaler.log" });
                        notification.setTitle("Trace logging still active");
                        ImGui::InsertNotification(notification);
                    }
                })
                .detach();
        }
#endif
        break;
    }
    case DLL_PROCESS_DETACH:
        State::Instance().isShuttingDown = true;
        if (skModule != nullptr)
            NtdllProxy::FreeLibrary_Ldr(skModule);
        if (reshadeModule != nullptr)
            NtdllProxy::FreeLibrary_Ldr(reshadeModule);
        if (!_asiHandles.empty())
        {
            for (size_t i = 0; i < _asiHandles.size(); ++i)
                NtdllProxy::FreeLibrary_Ldr(_asiHandles[i]);
        }
        for (const PVOID& module : State::Instance().modulesToFree)
            NtdllProxy::FreeLibrary_Ldr(module);
        spdlog::info("");
        spdlog::info("DLL_PROCESS_DETACH");
        spdlog::info("Unloading OptiScaler");
        CloseLogger();
        break;
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    default:
        LOG_WARN("Call reason: {0:X}", ul_reason_for_call);
        break;
    }

    return TRUE;
}
#endif
