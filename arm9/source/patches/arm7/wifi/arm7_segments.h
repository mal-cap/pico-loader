/* Splits a NitroSDK/TWL SDK game's ARM7 binary (as loaded, before crt0 runs)
 * into run-time segments for wl_locate. */
#ifndef ARM7_SEGMENTS_H
#define ARM7_SEGMENTS_H
#include <stdint.h>
#include "wl_locate.h"

typedef struct {
    uint32_t moduleParams;   /* run-time address of {list, listEnd, autoloadStart, ...} */
    uint32_t sdkVersion;     /* 0 if the ARM7 has no magic (SDK 2-4) */
    uint32_t entrySize;      /* autoload list entry: 12 (SDK 2-4) or 16 (SDK 5) */
    uint32_t listStart, listEnd, autoloadStart;
} arm7_layout_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the number of segments (static part first), or 0 on failure.
 * layout may be NULL. */
int arm7_segments(const uint8_t *arm7, uint32_t load, uint32_t size,
                  wl_segment_t *segs, int maxSegs, arm7_layout_t *layout);

#ifdef __cplusplus
}
#endif
#endif
