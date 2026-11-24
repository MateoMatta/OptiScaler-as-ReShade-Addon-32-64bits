#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

using AddonResourceKey = std::pair<uintptr_t, uint64_t>;

struct AddonSwapchainBufferEpoch
{
    AddonResourceKey resource;
    uint32_t index;
    uint64_t epoch;
};

using AddonSwapchainEpochMap = std::map<uintptr_t, std::vector<AddonSwapchainBufferEpoch>>;

struct AddonEpochMatch
{
    std::size_t count = 0;
    uint64_t epoch = 0;
};

inline AddonEpochMatch FindAddonSwapchainEpoch(const AddonSwapchainEpochMap& chains,
                                               const AddonResourceKey& resource)
{
    AddonEpochMatch result;
    for (const auto& chain : chains)
        for (const auto& buffer : chain.second)
            if (buffer.resource == resource)
            {
                ++result.count;
                result.epoch = buffer.epoch;
            }
    if (result.count != 1)
        result.epoch = 0;
    return result;
}

inline bool AddAddonSwapchainEpoch(AddonSwapchainEpochMap& chains, uintptr_t chain,
                                   const AddonSwapchainBufferEpoch& buffer)
{
    auto& buffers = chains[chain];
    for (const auto& existing : buffers)
        if (existing.index == buffer.index)
            return false;
    buffers.push_back(buffer);
    return true;
}
