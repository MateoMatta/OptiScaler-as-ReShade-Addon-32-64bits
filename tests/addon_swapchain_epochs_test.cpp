#include "../OptiScaler/AddonSwapchainEpochs.h"
#include <cassert>
#include <cstdio>

int main()
{
    AddonSwapchainEpochMap chains;
    const AddonResourceKey resource { 10, 100 };
    auto match = FindAddonSwapchainEpoch(chains, resource);
    assert(match.count == 0 && match.epoch == 0);

    assert(AddAddonSwapchainEpoch(chains, 1, { resource, 0, 1 }));
    assert(!AddAddonSwapchainEpoch(chains, 1, { { 10, 200 }, 0, 2 }));
    match = FindAddonSwapchainEpoch(chains, resource);
    assert(match.count == 1 && match.epoch == 1);
    assert(AddAddonSwapchainEpoch(chains, 2, { { 20, 100 }, 0, 2 }));
    assert(FindAddonSwapchainEpoch(chains, resource).epoch == 1);
    assert(FindAddonSwapchainEpoch(chains, { 20, 100 }).epoch == 2);

    assert(AddAddonSwapchainEpoch(chains, 2, { resource, 1, 3 }));
    match = FindAddonSwapchainEpoch(chains, resource);
    assert(match.count == 2 && match.epoch == 0);
    chains.erase(1);
    match = FindAddonSwapchainEpoch(chains, resource);
    assert(match.count == 1 && match.epoch == 3);
    chains.erase(2);
    assert(FindAddonSwapchainEpoch(chains, resource).count == 0);

    assert(AddAddonSwapchainEpoch(chains, 1, { resource, 0, 4 }));
    match = FindAddonSwapchainEpoch(chains, resource);
    assert(match.count == 1 && match.epoch == 4);
    std::puts("PASS unknown, duplicate slot, two devices, ambiguous overlap, destruction and handle reuse");
}
