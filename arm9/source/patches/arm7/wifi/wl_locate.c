/* See wl_locate.h. */
#include "wl_locate.h"

typedef uint32_t u32;
typedef uint8_t u8;

/* ---------------------------------------------------------------- memory -- */

typedef struct {
    const wl_segment_t *segs;
    int n;
} img_t;

static const u8 *ptr(const img_t *m, u32 addr, u32 len)
{
    for (int i = 0; i < m->n; i++) {
        const wl_segment_t *s = &m->segs[i];
        if (addr >= s->addr && addr - s->addr + len <= s->size)
            return s->data + (addr - s->addr);
    }
    return 0;
}

static int rd32(const img_t *m, u32 addr, u32 *out)
{
    const u8 *p = ptr(m, addr, 4);
    if (!p || (addr & 3))
        return 0;
    *out = wl_le32(p);
    return 1;
}

static u32 w32(const img_t *m, u32 addr)
{
    u32 v = 0;
    rd32(m, addr, &v);
    return v;
}

static int seg_of(const img_t *m, u32 addr)
{
    for (int i = 0; i < m->n; i++)
        if (addr >= m->segs[i].addr && addr - m->segs[i].addr < m->segs[i].size)
            return i;
    return -1;
}

/* SDK functions end up behind veneers when caller and callee are in different
 * autoload sections (VRAM -> WRAM): `ldr pc,[pc,#-4]; .word target`, or the
 * interworking form `ldr ip,[pc]; bx ip; .word target`. */
static u32 bl_target(u32 pc, u32 w)
{
    u32 off = w & 0x00FFFFFFu;
    if (off & 0x00800000u)
        off |= 0xFF000000u;
    return pc + 8 + (off << 2);
}

static u32 resolve(const img_t *m, u32 target)
{
    u32 a = w32(m, target);
    if (a == WL_ARM_LDR_PC_PC_M4)
        return w32(m, target + 4);
    if (a == 0xE59FC000u && w32(m, target + 4) == 0xE12FFF1Cu)
        return w32(m, target + 8);
    return target;
}

/* Nearest preceding `stmdb sp!, {..., lr}`. */
static u32 func_start(const img_t *m, u32 addr)
{
    for (u32 a = addr & ~3u, n = 0; n < 0x200; a -= 4, n++) {
        u32 w;
        if (!rd32(m, a, &w))
            return 0;
        if ((w & 0xFFFF4000u) == 0xE92D4000u)
            return a;
    }
    return 0;
}

/* ------------------------------------------------------------- simulator -- */

enum { V_UNK, V_CONST, V_ARG, V_FIELD, V_RET };

typedef struct {
    u8 kind;
    u8 arg;     /* V_ARG / V_FIELD: which incoming argument */
    u32 v;      /* V_CONST: value; V_FIELD: offset; V_RET: call index */
} val_t;

#define MAX_CALLS  48
#define MAX_STORES 64

typedef struct {
    u32 pc, target;         /* target: direct bl destination (not resolved) */
    val_t r[4], slot[2];    /* r0-r3 and [sp,#0], [sp,#4] at the call */
} call_t;

typedef struct {
    u32 pc;
    val_t base;             /* Rn */
    u32 imm;
    val_t val;              /* Rd */
} store_t;

/* About 4 KB, so each user keeps one static. ncalls keeps counting past
 * MAX_CALLS (return values are tagged with their call's index), so calls[]
 * is only read below MAX_CALLS. */
typedef struct {
    call_t calls[MAX_CALLS];
    int ncalls;
    store_t stores[MAX_STORES];
    int nstores;
} sim_t;

static int same(val_t a, val_t b)
{
    return a.kind == b.kind && a.kind != V_UNK && a.arg == b.arg && a.v == b.v;
}

static val_t vconst(u32 v) { val_t x = { V_CONST, 0, v }; return x; }
static val_t vunk(void) { val_t x = { V_UNK, 0, 0 }; return x; }

static u32 ror32(u32 v, u32 r) { r &= 31; return r ? (v >> r) | (v << (32 - r)) : v; }

/* Walks a function in a straight line (branches ignored) until it returns or
 * maxInsns. Good enough for the SDK's straight-line init and wrapper code. */
static void simulate(const img_t *m, u32 start, int maxInsns, sim_t *s)
{
    val_t r[16], slot[8];
    for (int i = 0; i < 16; i++)
        r[i] = vunk();
    for (int i = 0; i < 4; i++) {
        r[i].kind = V_ARG;
        r[i].arg = (u8)i;
    }
    for (int i = 0; i < 8; i++)
        slot[i] = vunk();
    s->ncalls = s->nstores = 0;

    for (u32 pc = start; maxInsns-- > 0; pc += 4) {
        u32 w;
        if (!rd32(m, pc, &w))
            return;
        u32 cond = w >> 28;
        if (cond == 0xF)
            continue;
        u32 rn = (w >> 16) & 15, rd = (w >> 12) & 15;

        if ((w & 0x0F000000u) == 0x0B000000u) {             /* BL */
            if (s->ncalls < MAX_CALLS) {
                call_t *c = &s->calls[s->ncalls];
                c->pc = pc;
                c->target = bl_target(pc, w);
                for (int i = 0; i < 4; i++)
                    c->r[i] = r[i];
                c->slot[0] = slot[0];
                c->slot[1] = slot[1];
            }
            r[0].kind = V_RET;
            r[0].v = (u32)s->ncalls;
            s->ncalls++;
            r[1] = r[2] = r[3] = r[12] = vunk();
            continue;
        }
        if ((w & 0x0F000000u) == 0x0A000000u)               /* B */
            continue;
        if ((w & 0x0FFFFFF0u) == 0x012FFF10u) {             /* BX */
            if (cond == 0xE && (w & 15) == 14)
                return;
            continue;
        }
        if ((w & 0x0E000000u) == 0x08000000u) {             /* LDM/STM */
            if (w & (1u << 20)) {
                if (cond == 0xE && rn == 13 && (w & 0x8000u))
                    return;                                  /* pop {..., pc} */
                for (int i = 0; i < 16; i++)
                    if (w & (1u << i))
                        r[i] = vunk();
            }
            continue;
        }
        if ((w & 0x0E000090u) == 0x00000090u && (w & 0x60u)) { /* LDRH/STRH etc. */
            if (w & (1u << 20))
                r[rd] = vunk();
            continue;
        }
        if ((w & 0x0FC000F0u) == 0x00000090u || (w & 0x0F8000F0u) == 0x00800090u) { /* MUL/MLA/UMULL */
            r[rn] = vunk();
            r[rd] = vunk();
            continue;
        }
        if ((w & 0x0C000000u) == 0x00000000u) {             /* data processing */
            u32 op = (w >> 21) & 15;
            val_t op2;
            if (w & (1u << 25))
                op2 = vconst(ror32(w & 0xFF, ((w >> 8) & 15) * 2));
            else if ((w & 0xFF0u) == 0)
                op2 = r[w & 15];
            else
                op2 = vunk();
            if (op >= 8 && op <= 11)                         /* TST/TEQ/CMP/CMN */
                continue;
            val_t res = vunk();
            if (cond == 0xE) {
                if (op == 0xD)
                    res = op2;
                else if (op == 0xF && op2.kind == V_CONST)
                    res = vconst(~op2.v);
                else if ((op == 4 || op == 2) && r[rn].kind == V_CONST && op2.kind == V_CONST)
                    res = vconst(op == 4 ? r[rn].v + op2.v : r[rn].v - op2.v);
            }
            r[rd] = res;
            continue;
        }
        if ((w & 0x0C000000u) == 0x04000000u) {             /* LDR/STR */
            int load = (w >> 20) & 1, byte = (w >> 22) & 1, regOff = (w >> 25) & 1;
            int pre = (w >> 24) & 1, up = (w >> 23) & 1, wb = (w >> 21) & 1;
            u32 imm = w & 0xFFF;
            if (load) {
                val_t res = vunk();
                if (cond == 0xE && !regOff && pre && !wb && !byte) {
                    if (rn == 15) {
                        u32 lit;
                        if (rd32(m, up ? pc + 8 + imm : pc + 8 - imm, &lit))
                            res = vconst(lit);
                    } else if (rn == 13 && up && imm < 32) {
                        res = slot[imm / 4];
                    } else if (r[rn].kind == V_ARG && up) {
                        res.kind = V_FIELD;
                        res.arg = r[rn].arg;
                        res.v = imm;
                    }
                }
                if (wb || !pre)
                    r[rn] = vunk();
                r[rd] = res;
            } else if (!byte && !regOff && pre && up) {
                if (rn == 13 && imm < 32)
                    slot[imm / 4] = r[rd];
                else if (s->nstores < MAX_STORES) {
                    store_t *st = &s->stores[s->nstores++];
                    st->pc = pc;
                    st->base = r[rn];
                    st->imm = imm;
                    st->val = r[rd];
                }
            }
            continue;
        }
        /* anything else: coprocessor, SWI, ... leave registers alone */
    }
}

/* ------------------------------------------------------------- scanning -- */

/* Calls fn(addr) for every aligned word in every segment equal to `value`
 * (under `mask`); stops early if fn returns nonzero. */
typedef int (*scan_fn)(const img_t *m, u32 addr, void *ctx);

static int scan(const img_t *m, u32 mask, u32 value, scan_fn fn, void *ctx)
{
    for (int i = 0; i < m->n; i++) {
        const wl_segment_t *s = &m->segs[i];
        for (u32 off = 0; off + 4 <= s->size; off += 4) {
            const u8 *p = s->data + off;
            u32 w = wl_le32(p);
            if ((w & mask) == value) {
                int r = fn(m, s->addr + off, ctx);
                if (r)
                    return r;
            }
        }
    }
    return 0;
}

/* --- rule 1: WMSP_WlRequest --------------------------------------------------
 *   OS_SendMessage(&toWL, req, BLOCK); OS_ReceiveMessage(&confirm, &msg, BLOCK);
 *   if (msg->cfm.resultCode == 14) ...     <- this body is the anchor:
 *   ldrh r0,[r1,#14]; add r0,r1,r0,lsl #1; ldrh r0,[r0,#20]; cmp r0,#14 */
static const u32 kWlRequestBody[4] = { 0xE1D100BEu, 0xE0810080u, 0xE1D001B4u, 0xE350000Eu };

static int on_wlrequest(const img_t *m, u32 addr, void *ctx)
{
    wl_locate_result_t *o = ctx;
    for (int i = 1; i < 4; i++)
        if (w32(m, addr + 4 * i) != kWlRequestBody[i])
            return 0;
    u32 start = func_start(m, addr);
    if (!start)
        return 0;
    static sim_t s;
    simulate(m, start, 24, &s);
    if (s.ncalls < 2)
        return 0;
    call_t *snd = &s.calls[0], *rcv = &s.calls[1];
    if (snd->r[0].kind != V_CONST || !same(snd->r[2], vconst(1)) ||
        rcv->r[0].kind != V_CONST || !same(rcv->r[2], vconst(1)))
        return 0;
    o->wlRequest = start;
    o->sendMessage = resolve(m, snd->target);
    o->receiveMessage = resolve(m, rcv->target);
    o->toWlQueue = snd->r[0].v;
    o->confirmQueue = rcv->r[0].v;
    return 1;
}

/* --- rule 2: WMSP_InitWL ------------------------------------------------------
 *   OS_InitMessageQueue x4 with depths 2 (toWL), 4 (fromWL), 4 (confirm),
 *   32 (request); stores the WL init param's heap id/handle (+36/+40) and the
 *   indicate priority into WM's work area; OS_CreateThread + OS_WakeupThreadDirect
 *   for the indicate thread; ends with `return WL_InitDriver(param)`. */
typedef struct {
    wl_locate_result_t *o;
    int err;
    u32 heapIdOff, heapHandleOff;  /* str immediates, for rule 3 */
    u32 wlInitDriver;
} initwl_ctx_t;

static int store_of(const sim_t *s, val_t v, u32 *addr, u32 *imm)
{
    for (int i = 0; i < s->nstores; i++) {
        const store_t *st = &s->stores[i];
        if (same(st->val, v) && st->base.kind == V_CONST) {
            *addr = st->base.v + st->imm;
            if (imm)
                *imm = st->imm;
            return 1;
        }
    }
    return 0;
}

static int on_initwl(const img_t *m, u32 addr, void *vctx)
{
    initwl_ctx_t *c = vctx;
    wl_locate_result_t *o = c->o;
    u32 start = func_start(m, addr);
    if (!start)
        return 0;
    static sim_t s;
    simulate(m, start, 200, &s);

    static const u32 depth[4] = { 2, 4, 4, 32 };
    int q[4], k = 0;
    for (int i = 0; i < s.ncalls && i < MAX_CALLS && k < 4; i++) {
        call_t *cl = &s.calls[i];
        if (same(cl->r[2], vconst(depth[k])) && cl->r[0].kind == V_CONST &&
            (k == 0 || cl->target == s.calls[q[0]].target))
            q[k++] = i;
    }
    if (k < 4)
        return 0;                       /* not InitWL; keep scanning */

    if (s.calls[q[0]].r[0].v != o->toWlQueue || s.calls[q[2]].r[0].v != o->confirmQueue) {
        c->err = WL_LOCATE_QUEUE_MISMATCH;
        return 1;
    }
    o->fromWlQueue = s.calls[q[1]].r[0].v;

    /* indicate thread: first call after the queue inits taking
     * (thread, func, arg, stackTop) with size/prio on the stack */
    int ct = -1;
    for (int i = q[3] + 1; i < s.ncalls && i < MAX_CALLS; i++) {
        call_t *cl = &s.calls[i];
        if (cl->r[0].kind == V_CONST && cl->r[1].kind == V_CONST && seg_of(m, cl->r[1].v) >= 0 &&
            cl->r[3].kind == V_CONST && cl->slot[0].kind == V_CONST) {
            ct = i;
            break;
        }
    }
    if (ct < 0 || ct + 1 >= s.ncalls || !same(s.calls[ct + 1].r[0], s.calls[ct].r[0])) {
        c->err = WL_LOCATE_NO_THREAD;
        return 1;
    }
    o->createThread = resolve(m, s.calls[ct].target);
    o->wakeupThreadDirect = resolve(m, s.calls[ct + 1].target);

    val_t heapId = { V_FIELD, 0, 36 }, heapHandle = { V_FIELD, 0, 40 };
    if (!store_of(&s, heapId, &o->heapIdAddr, &c->heapIdOff) ||
        !store_of(&s, heapHandle, &o->heapHandleAddr, &c->heapHandleOff)) {
        c->err = WL_LOCATE_NO_HEAP_VARS;
        return 1;
    }

    /* last call, with the incoming param in r0: WL_InitDriver */
    for (int i = s.ncalls - 1; i > ct && i >= 0; i--) {
        if (i < MAX_CALLS && same(s.calls[i].r[0], (val_t){ V_ARG, 0, 0 })) {
            c->wlInitDriver = resolve(m, s.calls[i].target);
            break;
        }
    }
    c->err = WL_LOCATE_OK;
    return 1;
}

/* --- rule 3: OS_AllocFromHeap / OS_FreeToHeap ---------------------------------
 *   ldr r0,[Rn,#heapIdOff]; ldr r1,[Rn,#heapHandleOff]; ... bl X
 *   with r2 a constant (size) -> alloc, r2 a register (pointer) -> free. */
typedef struct {
    u32 idOff, handleOff;
    u32 alloc, free;
    int conflict;
} heap_ctx_t;

static int on_heap_site(const img_t *m, u32 addr, void *vctx)
{
    heap_ctx_t *c = vctx;
    u32 w0 = w32(m, addr), w1 = w32(m, addr + 4);
    u32 rn = (w0 >> 16) & 15;
    if (w1 != (0xE5901000u | (rn << 16) | c->handleOff))
        return 0;
    int kind = 0; /* 1 alloc, 2 free */
    u32 target = 0;
    for (int i = -3; i < 6; i++) {
        u32 a = addr + 4 * (u32)i, w = w32(m, a);
        if ((w & 0xFFFFF000u) == 0xE3A02000u)
            kind = 1;                                   /* mov r2,#imm */
        else if ((w & 0xFFFFFFF0u) == 0xE1A02000u)
            kind = 2;                                   /* mov r2,Rm */
        if ((w & 0xFF000000u) == 0xEB000000u && i >= 2) {
            target = resolve(m, bl_target(a, w));
            break;
        }
    }
    if (!kind || !target)
        return 0;
    u32 *slot = kind == 1 ? &c->alloc : &c->free;
    if (*slot && *slot != target)
        c->conflict = 1;
    *slot = target;
    return 0;
}

/* True if the function at `start` has a conditional backward branch whose loop
 * body contains a call: OS_Sleep's `while (alarm pending) OS_SleepThread()`. */
static int has_call_loop(const img_t *m, u32 start)
{
    for (u32 pc = start, n = 0; n < 0x80; pc += 4, n++) {
        u32 w;
        if (!rd32(m, pc, &w))
            return 0;
        if (w == 0xE12FFF1Eu || (w & 0xFFFF8000u) == 0xE8BD8000u)
            return 0;                                   /* bx lr / pop {..pc} */
        if ((w & 0x0F000000u) == 0x0A000000u && (w >> 28) != 0xE && (w & 0x00800000u)) {
            u32 t = pc + 8 + ((w | 0xFF000000u) << 2);
            for (u32 a = t; a < pc; a += 4)
                if ((w32(m, a) & 0x0F000000u) == 0x0B000000u)
                    return 1;
        }
    }
    return 0;
}

/* True if the function makes a call with a code address in r3: OS_SetAlarm
 * (alarm, ticks64, handler, arg) with OS_Sleep's wake-up handler. */
static int sets_alarm_callback(const img_t *m, u32 start)
{
    static sim_t s;
    simulate(m, start, 0x60, &s);
    for (int i = 0; i < s.ncalls && i < MAX_CALLS; i++)
        if (s.calls[i].r[3].kind == V_CONST && seg_of(m, s.calls[i].r[3].v) >= 0)
            return 1;
    return 0;
}

/* --- rule 4: OS_Sleep ---------------------------------------------------------
 *   the function in the OS's section that uses OS_MilliSecondsToTicks'
 *   constant 0x82EA (33513982 Hz / 64 / 1000) and loops on a call
 *   (OS_SleepThread) after setting an alarm with a code callback in r3. WM/WL
 *   convert milliseconds too, and OS sections have other users: spin-waits
 *   and OS_GetTick timing loops. */
typedef struct {
    u32 func;
    int conflict;
    int seg;        /* only the OS's section (where OS_SendMessage lives) */
} sleep_ctx_t;

static int on_sleep_literal(const img_t *m, u32 lit, void *vctx)
{
    sleep_ctx_t *c = vctx;
    if (seg_of(m, lit) != c->seg)
        return 0;
    for (u32 a = lit - 4, n = 0; n < 0x400 && a >= 8; a -= 4, n++) {
        u32 w;
        if (!rd32(m, a, &w))
            break;
        /* ldr rX,[pc,#+imm] that loads `lit` */
        if ((w & 0xFFFF0000u) == 0xE59F0000u && a + 8 + (w & 0xFFF) == lit) {
            u32 f = func_start(m, a);
            if (!f || !has_call_loop(m, f) || !sets_alarm_callback(m, f))
                continue;
            if (c->func && c->func != f)
                c->conflict = 1;
            c->func = f;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- driver -- */

int wl_locate(const wl_segment_t *segs, int nsegs, wl_locate_result_t *o)
{
    img_t m = { segs, nsegs };
    for (u32 i = 0; i < sizeof(*o) / 4; i++)
        ((u32 *)o)[i] = 0;

    if (!scan(&m, 0xFFFFFFFFu, kWlRequestBody[0], on_wlrequest, o))
        return WL_LOCATE_NO_WLREQUEST;

    initwl_ctx_t ic = { o, WL_LOCATE_NO_INITWL, 0, 0, 0 };
    if (!scan(&m, 0xFFFFFFFFu, 0xE3A02020u /* mov r2,#32 */, on_initwl, &ic))
        return WL_LOCATE_NO_INITWL;
    if (ic.err)
        return ic.err;

    heap_ctx_t hc = { ic.heapIdOff, ic.heapHandleOff, 0, 0, 0 };
    scan(&m, 0xFFF0FFFFu, 0xE5900000u | ic.heapIdOff /* ldr r0,[Rn,#idOff] */, on_heap_site, &hc);
    if (!hc.alloc || !hc.free || hc.conflict)
        return WL_LOCATE_NO_HEAP_FUNCS;
    o->allocFromHeap = hc.alloc;
    o->freeToHeap = hc.free;

    sleep_ctx_t sc = { 0, 0, seg_of(&m, o->sendMessage) };
    scan(&m, 0xFFFFFFFFu, 0x000082EAu, on_sleep_literal, &sc);
    if (!sc.func || sc.conflict)
        return WL_LOCATE_NO_SLEEP;
    o->sleep = sc.func;

    /* OS_GetLockID: WL_InitDriver's second call (the first is a CpuFill of its
     * work area); its result is WL's lock id. */
    if (!ic.wlInitDriver)
        return WL_LOCATE_NO_LOCKID;
    static sim_t s;
    simulate(&m, ic.wlInitDriver, 32, &s);
    if (s.ncalls < 2)
        return WL_LOCATE_NO_LOCKID;
    o->getLockId = resolve(&m, s.calls[1].target);
    if (seg_of(&m, o->getLockId) == seg_of(&m, ic.wlInitDriver))
        return WL_LOCATE_NO_LOCKID;     /* expected to live in the OS (WRAM) */
    return WL_LOCATE_OK;
}

const char *wl_locate_error(int err)
{
    static const char *const msg[] = {
        "ok",
        "WMSP_WlRequest not found",
        "WMSP_InitWL not found",
        "InitWL and WlRequest disagree on the queues",
        "indicate thread creation not decoded",
        "WM heap variables not found",
        "OS_AllocFromHeap/OS_FreeToHeap not found",
        "OS_Sleep not found or not unique",
        "OS_GetLockID not found",
    };
    return err >= 0 && err < (int)(sizeof(msg) / sizeof(msg[0])) ? msg[err] : "?";
}
