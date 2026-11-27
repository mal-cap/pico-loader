#include "common.h"
#include <string.h>
#include "arm7_segments.h"
#include "WlProxyPatch.h"

bool WlProxyPatch::Locate(const nds_header_ntr_t* romHeader)
{
    static wl_segment_t segments[WL_LOCATE_MAX_SEGMENTS];
    int count = arm7_segments((const u8*)romHeader->arm7LoadAddress, romHeader->arm7LoadAddress,
        romHeader->arm7Size, segments, WL_LOCATE_MAX_SEGMENTS, nullptr);
    if (count == 0)
    {
        LOG_DEBUG("Wi-Fi proxy: arm7 layout not found\n");
        return false;
    }

    int error = wl_locate(segments, count, &_locations);
    if (error != WL_LOCATE_OK)
    {
        LOG_DEBUG("Wi-Fi proxy: %s\n", wl_locate_error(error));
        return false;
    }

    // The hook is written in the segment's load image (autoloads have not been copied yet).
    for (int i = 0; i < count; i++)
    {
        if (_locations.wlRequest >= segments[i].addr &&
            _locations.wlRequest + 8 <= segments[i].addr + segments[i].size)
        {
            _hook = (u32*)(segments[i].data + (_locations.wlRequest - segments[i].addr));
        }
    }
    if (_hook == nullptr || _hook[0] == WL_ARM_LDR_PC_PC_M4)
    {
        LOG_DEBUG("Wi-Fi proxy: hook point unusable\n");
        _hook = nullptr;
        return false;
    }

    LOG_DEBUG("Wi-Fi proxy: WMSP_WlRequest at 0x%x\n", _locations.wlRequest);
    return true;
}

void WlProxyPatch::ApplyPatch(PatchContext& patchContext)
{
    if (_hook == nullptr)
        return;

    u8* target = (u8*)_target;
    memcpy(target, _blob->image, _blob->loadSize);
    for (u32 i = 0; i < _blob->relocCount; i++)
    {
        *(u32*)(target + _blob->relocs[i]) += (u32)_target;
    }
    memcpy(target + _blob->cfgOffset, &_locations, sizeof(_locations));
    memset(target + _blob->loadSize, 0, _blob->bssSize);

    _hook[0] = WL_ARM_LDR_PC_PC_M4;
    _hook[1] = (u32)_target;

    LOG_DEBUG("Wi-Fi proxy at 0x%p (%d + %d bytes)\n", _target, _blob->loadSize, _blob->bssSize);
}
