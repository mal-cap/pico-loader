#pragma once
#include "patches/Patch.h"
#include "ndsHeader.h"
#include "WlProxyBlob.h"
#include "wl_locate.h"

/// @brief Arm7 patch that installs the Wi-Fi proxy: it replaces WMSP_WlRequest, adds an
///        access point that leads to the platform's network link, and bridges its traffic.
///        The addresses it needs are found by wl_locate, which works by what the code does
///        rather than by exact byte patterns.
class WlProxyPatch final : public Patch
{
public:
    /// @param blob The proxy built for the platform.
    /// @param target Where the proxy will run, in the arm7 main memory arena.
    WlProxyPatch(const WlProxyBlob* blob, void* target)
        : _blob(blob), _target(target) { }

    /// @brief Finds the hook point and the addresses the proxy needs. Called before the
    ///        arena space is reserved, so that games without WFC support lose nothing.
    /// @param romHeader The rom header, with the arm7 already loaded.
    /// @return \c true if the game can take the proxy, or \c false otherwise.
    bool Locate(const nds_header_ntr_t* romHeader);

    /// @brief Returns the bytes the proxy takes from the arena (code and bss, 32-byte aligned).
    u32 GetSize() const { return (_blob->loadSize + _blob->bssSize + 31) & ~31u; }

    /// @brief The work was done by \see Locate; the patch is only added when that succeeded.
    bool FindPatchTarget(PatchContext& patchContext) override { return _hook != nullptr; }

    void ApplyPatch(PatchContext& patchContext) override;

private:
    const WlProxyBlob* _blob;
    void* _target;
    u32* _hook = nullptr;
    wl_locate_result_t _locations;
};
