#pragma once

/// @brief A Wi-Fi proxy image for the game's arm7, built for one platform's network link.
///        The image is linked at address 0; \see relocs lists the words that need the
///        load address added. Generated from the proxy's ELF by tools/proxy_blob.py.
struct WlProxyBlob
{
    /// @brief Code and data, \see loadSize bytes.
    const u32* image;

    /// @brief Bytes of \see image to copy.
    u32 loadSize;

    /// @brief Zeroed bytes after the image.
    u32 bssSize;

    /// @brief Offset of the configuration block (a wl_locate_result_t).
    u32 cfgOffset;

    /// @brief Offsets of the words to relocate.
    const u16* relocs;

    /// @brief Number of entries in \see relocs.
    u32 relocCount;

    /// @brief Returns the proxy built into this loader, or \c nullptr if there is none.
    static const WlProxyBlob* Get();
};
