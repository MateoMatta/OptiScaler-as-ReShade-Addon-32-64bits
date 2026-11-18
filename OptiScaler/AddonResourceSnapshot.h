#pragma once

#include <reshade_api.hpp>

enum class AddonViewSnapshotStatus
{
    null_view,
    no_resource,
    non_texture_2d,
    texture_2d
};

struct AddonViewSnapshot
{
    AddonViewSnapshotStatus status = AddonViewSnapshotStatus::null_view;
    reshade::api::resource_view view {};
    reshade::api::resource resource {};
    reshade::api::resource_view_desc view_desc {};
    reshade::api::resource_desc resource_desc {};
};

template <typename Device>
AddonViewSnapshot InspectAddonView(Device* device, reshade::api::resource_view view)
{
    AddonViewSnapshot snapshot {};
    snapshot.view = view;
    if (view.handle == 0)
        return snapshot;

    snapshot.resource = device->get_resource_from_view(view);
    if (snapshot.resource.handle == 0)
    {
        snapshot.status = AddonViewSnapshotStatus::no_resource;
        return snapshot;
    }

    snapshot.view_desc = device->get_resource_view_desc(view);
    snapshot.resource_desc = device->get_resource_desc(snapshot.resource);
    snapshot.status = snapshot.resource_desc.type == reshade::api::resource_type::texture_2d
                          ? AddonViewSnapshotStatus::texture_2d
                          : AddonViewSnapshotStatus::non_texture_2d;
    return snapshot;
}
