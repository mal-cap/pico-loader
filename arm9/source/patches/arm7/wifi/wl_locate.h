/*
 * Finds, in a NitroSDK game's ARM7, everything the WL proxy needs: the hook
 * point (WMSP_WlRequest), the ARM7 OS functions it calls, and WM's message
 * queues and heap.
 *
 * Portable C, no allocation, no libc beyond <stdint.h>. Works on the ARM7 as
 * loaded, before the game's crt0 has copied its autoload sections: each
 * segment is a run-time address range plus a pointer to its bytes.
 *
 * The rules are described in wl_locate.c. They are semantic (what the SDK
 * code does), not byte signatures, so compiler scheduling differences
 * between SDK builds don't break them.
 */
#ifndef WL_LOCATE_H
#define WL_LOCATE_H

#include <stdint.h>

#define WL_LOCATE_MAX_SEGMENTS 8

/* ldr pc,[pc,#-4]: the first word of an SDK veneer, and of the proxy's hook. */
#define WL_ARM_LDR_PC_PC_M4 0xE51FF004u

static inline uint32_t wl_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    uint32_t addr;          /* run-time address */
    uint32_t size;          /* bytes of code/data (BSS not included) */
    const uint8_t *data;    /* where the bytes are now */
} wl_segment_t;

typedef struct {
    /* hook point */
    uint32_t wlRequest;         /* WMSP_WlRequest (ARM) */
    /* ARM7 OS functions (run-time addresses, ARM) */
    uint32_t sendMessage;
    uint32_t receiveMessage;
    uint32_t createThread;
    uint32_t wakeupThreadDirect;
    uint32_t sleep;
    uint32_t getLockId;
    uint32_t allocFromHeap;
    uint32_t freeToHeap;
    /* WM data (run-time addresses) */
    uint32_t toWlQueue;
    uint32_t fromWlQueue;
    uint32_t confirmQueue;
    uint32_t heapIdAddr;        /* holds the WM heap's arena id */
    uint32_t heapHandleAddr;    /* holds the WM heap handle */
} wl_locate_result_t;

/* Error codes: which rule failed. 0 = success. */
enum {
    WL_LOCATE_OK = 0,
    WL_LOCATE_NO_WLREQUEST,     /* WMSP_WlRequest body not found */
    WL_LOCATE_NO_INITWL,        /* no function with the 2/4/4/32 queue inits */
    WL_LOCATE_QUEUE_MISMATCH,   /* InitWL's queues disagree with WlRequest's */
    WL_LOCATE_NO_THREAD,        /* CreateThread/WakeupThreadDirect not decoded */
    WL_LOCATE_NO_HEAP_VARS,     /* heap id/handle variables not found */
    WL_LOCATE_NO_HEAP_FUNCS,    /* OS_AllocFromHeap/OS_FreeToHeap not found */
    WL_LOCATE_NO_SLEEP,         /* OS_Sleep not found (or not unique) */
    WL_LOCATE_NO_LOCKID,        /* OS_GetLockID not found */
};

#ifdef __cplusplus
extern "C" {
#endif

int wl_locate(const wl_segment_t *segs, int nsegs, wl_locate_result_t *out);
const char *wl_locate_error(int err);

#ifdef __cplusplus
}
#endif

#endif
