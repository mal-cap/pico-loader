/* See arm7_segments.h. */
#include "arm7_segments.h"

#define rd wl_le32

int arm7_segments(const uint8_t *a7, uint32_t load, uint32_t size,
                  wl_segment_t *segs, int maxSegs, arm7_layout_t *L)
{
    arm7_layout_t local;
    if (!L)
        L = &local;
    uint32_t end = load + size, mp = 0;
    L->moduleParams = L->sdkVersion = 0;
    L->entrySize = 12;

    /* SDK 5 ARM7s carry the module-params magic: {list, listEnd, autoloadStart,
     * bssStart, bssEnd, compressedEnd, sdkVersion, 0xDEC00621, 0x2106C0DE}. */
    for (uint32_t o = 0x1C; o + 8 <= size && o < 0x2000; o += 4) {
        if (rd(a7 + o) == 0xDEC00621u && rd(a7 + o + 4) == 0x2106C0DEu) {
            mp = o - 0x1C;
            L->sdkVersion = rd(a7 + mp + 0x18);
            if (L->sdkVersion >= 0x05000000u)
                L->entrySize = 16;
            break;
        }
    }
    /* Older ARM7 crt0s have no magic: look for a plausible triple near the start. */
    if (!mp) {
        for (uint32_t o = 0; o + 12 <= size && o < 0x400; o += 4) {
            uint32_t s = rd(a7 + o), e = rd(a7 + o + 4), a = rd(a7 + o + 8);
            if (load <= a && a < s && s < e && e <= end && (e - s) % 12 == 0 && (e - s) / 12 < 8) {
                mp = o;
                break;
            }
        }
    }
    if (!mp)
        return 0;
    L->moduleParams = load + mp;
    L->listStart = rd(a7 + mp);
    L->listEnd = rd(a7 + mp + 4);
    L->autoloadStart = rd(a7 + mp + 8);
    if (L->listStart < load || L->listEnd > end || L->listStart > L->listEnd ||
        L->autoloadStart < load || L->autoloadStart > L->listStart ||
        (L->listEnd - L->listStart) % L->entrySize)
        return 0;

    int n = 0;
    segs[n++] = (wl_segment_t){ load, L->autoloadStart - load, a7 };
    uint32_t src = L->autoloadStart - load;
    for (uint32_t e = L->listStart; e < L->listEnd && n < maxSegs; e += L->entrySize) {
        const uint8_t *p = a7 + (e - load);
        uint32_t dst = rd(p), sz = rd(p + 4);
        if (src + sz > size)
            return 0;
        segs[n++] = (wl_segment_t){ dst, sz, a7 + src };
        src += sz;
    }
    return n;
}
