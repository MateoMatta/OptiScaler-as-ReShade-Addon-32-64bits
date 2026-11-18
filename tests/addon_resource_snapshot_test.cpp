#include "../OptiScaler/AddonResourceSnapshot.h"
#include <cassert>

struct FakeDevice
{
    reshade::api::resource resource {};
    reshade::api::resource_desc desc {};
    reshade::api::resource_view_desc view_desc {};
    unsigned resource_queries = 0;
    unsigned view_queries = 0;
    unsigned desc_queries = 0;

    reshade::api::resource get_resource_from_view(reshade::api::resource_view)
    {
        ++resource_queries;
        return resource;
    }

    reshade::api::resource_view_desc get_resource_view_desc(reshade::api::resource_view)
    {
        ++view_queries;
        return view_desc;
    }

    reshade::api::resource_desc get_resource_desc(reshade::api::resource)
    {
        ++desc_queries;
        return desc;
    }
};

int main()
{
    FakeDevice device {};
    const reshade::api::resource_view view { 42 };
    auto snapshot = InspectAddonView(&device, {});
    assert(snapshot.status == AddonViewSnapshotStatus::null_view);
    assert(device.resource_queries == 0);

    snapshot = InspectAddonView(&device, view);
    assert(snapshot.status == AddonViewSnapshotStatus::no_resource);
    assert(device.view_queries == 0 && device.desc_queries == 0);

    device.resource.handle = 123;
    device.desc.type = reshade::api::resource_type::buffer;
    snapshot = InspectAddonView(&device, view);
    assert(snapshot.status == AddonViewSnapshotStatus::non_texture_2d);
    assert(snapshot.resource.handle == 123 && snapshot.view.handle == 42);

    device.desc.type = reshade::api::resource_type::texture_2d;
    device.desc.texture.width = 1920;
    device.desc.texture.height = 1080;
    device.view_desc = reshade::api::resource_view_desc(
        reshade::api::resource_view_type::texture_2d, reshade::api::format::r8g8b8a8_unorm, 1, 1, 0, 1);
    snapshot = InspectAddonView(&device, view);
    assert(snapshot.status == AddonViewSnapshotStatus::texture_2d);
    assert(snapshot.resource_desc.texture.width == 1920 && snapshot.resource_desc.texture.height == 1080);
    assert(snapshot.view_desc.format == reshade::api::format::r8g8b8a8_unorm);
    assert(snapshot.view_desc.texture.first_level == 1);
    assert(device.resource_queries == 3 && device.view_queries == 2 && device.desc_queries == 2);
}
