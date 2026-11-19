#include <reshade.hpp>
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static reshade::addon_event_traits<reshade::addon_event::present>::decl presentCallback = nullptr;

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void*, uint32_t version)
{
    return version == RESHADE_API_VERSION;
}

extern "C" __declspec(dllexport) bool ReShadeUnregisterAddon(void*)
{
    return true;
}

extern "C" __declspec(dllexport) void ReShadeRegisterEvent(reshade::addon_event event, void* callback)
{
    if (event == reshade::addon_event::present)
        presentCallback = reinterpret_cast<decltype(presentCallback)>(callback);
}

extern "C" __declspec(dllexport) void ReShadeUnregisterEvent(reshade::addon_event, void*)
{
}

static std::vector<DWORD> currentThreadIds()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};
    std::vector<DWORD> ids;
    THREADENTRY32 entry { sizeof(entry) };
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == GetCurrentProcessId())
                ids.push_back(entry.th32ThreadID);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return ids;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2 || argc > 3 || (argc == 3 && wcscmp(argv[2], L"--pause-worker") != 0))
    {
        std::fwprintf(stderr, L"Usage: addon_logger_stress <isolated addon path> [--pause-worker]\n");
        return 1;
    }

    HMODULE addon = LoadLibraryW(argv[1]);
    if (addon == nullptr)
    {
        std::fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    using Init = bool (*)(HMODULE, HMODULE);
    using Uninit = void (*)(HMODULE, HMODULE);
    const auto init = reinterpret_cast<Init>(GetProcAddress(addon, "AddonInit"));
    const auto uninit = reinterpret_cast<Uninit>(GetProcAddress(addon, "AddonUninit"));
    HMODULE mock = GetModuleHandleW(nullptr);
    const auto threadsBefore = currentThreadIds();
    if (init == nullptr || uninit == nullptr || !init(addon, mock))
    {
        std::fwprintf(stderr, L"Add-on initialization failed\n");
        FreeLibrary(addon);
        return 1;
    }
    if (presentCallback == nullptr)
    {
        std::fwprintf(stderr, L"Present callback not registered\n");
        uninit(addon, mock);
        FreeLibrary(addon);
        return 1;
    }

    HANDLE worker = nullptr;
    if (argc == 3)
    {
        const auto threadsAfter = currentThreadIds();
        std::vector<DWORD> added;
        for (const DWORD id : threadsAfter)
            if (std::find(threadsBefore.begin(), threadsBefore.end(), id) == threadsBefore.end())
                added.push_back(id);
        if (threadsBefore.empty() || added.size() != 1 ||
            (worker = OpenThread(THREAD_SUSPEND_RESUME, FALSE, added[0])) == nullptr)
        {
            std::fwprintf(stderr, L"Could not identify the isolated logger worker\n");
            uninit(addon, mock);
            FreeLibrary(addon);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (SuspendThread(worker) == DWORD(-1))
        {
            std::fwprintf(stderr, L"Could not suspend logger worker: %lu\n", GetLastError());
            CloseHandle(worker);
            uninit(addon, mock);
            FreeLibrary(addon);
            return 1;
        }
    }

    constexpr int threadCount = 8;
    constexpr int eventsPerThread = 20000;
    std::vector<std::thread> threads;
    for (int i = 0; i < threadCount; ++i)
        threads.emplace_back([eventsPerThread] {
            for (int j = 0; j < eventsPerThread; ++j)
                presentCallback(nullptr, nullptr, nullptr, nullptr, 0, nullptr);
        });
    if (worker != nullptr)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (ResumeThread(worker) == DWORD(-1))
        {
            std::fwprintf(stderr, L"Could not resume logger worker: %lu\n", GetLastError());
            std::terminate();
        }
        CloseHandle(worker);
    }
    for (auto& thread : threads)
        thread.join();

    uninit(addon, mock);
    FreeLibrary(addon);
    std::printf("sent=%d\n", threadCount * eventsPerThread);
    return 0;
}
