/*
 * WL request proxy: fake access point + DSpico card transport.
 *
 * Replaces WMSP_WlRequest in the game's ARM7. Every request the WM library
 * sends to the 802.11 driver (WL) arrives here. Requests that concern the
 * fake access point are answered locally; everything else goes to the real
 * driver exactly as the original function did.
 *
 *   TX: MA_DATA while joined -> Ethernet II in txbuf; the pump thread sends
 *       it with NET_TX.
 *   RX: the pump thread polls NET_STATUS every 1 ms while joined, reads
 *       frames with NET_RX, wraps each in an MA_DATA_IND allocated from the
 *       WM heap and posts it to WM's fromWL queue, exactly as the real WL does.
 *
 * Only the pump thread touches the card. It runs below the card task, so it
 * never preempts it: the task's save code (Pico Loader's) uses the bus
 * without the card lock and busy-waits until each transfer is done. Each
 * network command runs with interrupts off, so nothing preempts it either.
 *
 * Per-game addresses come from gProxyCfg, which WlProxyPatch fills in using
 * wl_locate (arm9/source/patches/arm7/wifi/). Layouts are the SDK's
 * WlCmd.h/WlFrame.h, confirmed against the Mario Kart DS (USA) and Pokemon
 * Platinum ARM7s. Offsets are bytes from the start of the WL buffer (6
 * reserved halfwords, then the WlCmdHeader).
 */
#include <stdint.h>
#include "proxy_cfg.h"
#include "dspico_card7.h"
#include "dspico_net_proto.h"
#include "frame.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* --- WL protocol ----------------------------------------------------------- */
#define REQ_CODE        12
#define REQ_LEN         14
#define REQ_BODY        16

#define WL_MLME_RESET   0x0000
#define WL_MLME_SCAN    0x0002
#define WL_MLME_JOIN    0x0003
#define WL_MLME_AUTH    0x0004
#define WL_MLME_DEAUTH  0x0005
#define WL_MLME_ASS     0x0006
#define WL_MLME_DISASS  0x0008
#define WL_MA_DATA      0x0100
#define WL_MA_CLRDATA   0x0104
#define WL_MA_DATA_IND  0x0180

#define SCAN_BSSID      (REQ_BODY + 0)
#define SCAN_SSIDLEN    (REQ_BODY + 6)
#define SCAN_SSID       (REQ_BODY + 8)
#define SCAN_CHLIST     (REQ_BODY + 42)
#define JOIN_BSSID      (REQ_BODY + 4 + 4)
#define PEER_MAC        (REQ_BODY + 0)
#define AUTH_ALGO       (REQ_BODY + 6)
/* MA_DATA request: WlTxFrame at REQ_BODY */
#define TX_LENGTH       (REQ_BODY + 6)
#define TX_DEST         (REQ_BODY + 24)
#define TX_SRC          (REQ_BODY + 30)
#define TX_DATAP        (REQ_BODY + 44)
/* MA_DATA_IND: WlRxFrame at REQ_BODY, 44-byte header then data */
#define IND_FRAME       REQ_BODY
#define RXF_LENGTH      6
#define RXF_RATE        14
#define RXF_RSSI        15
#define RXF_DEST        24
#define RXF_SRC         30
#define RXF_DATA        44
#define IND_HEADER_BYTES (REQ_BODY + RXF_DATA)    /* 60 */

/* --- game OS functions (addresses from the patcher, see proxy_cfg.h) ------- */
typedef int (*msg_fn)(void *queue, void *msg, int flags);
typedef void (*thread_fn)(void *);

/* Placed with the code so it is part of the loaded blob. */
__attribute__((section(".data.proxy_cfg"), used)) wl_locate_result_t gProxyCfg = { 0 };

#define OS_SendMessage      ((msg_fn)gProxyCfg.sendMessage)
#define OS_ReceiveMessage   ((msg_fn)gProxyCfg.receiveMessage)
#define OS_CreateThread     ((void (*)(void *, thread_fn, void *, void *, u32, u32))gProxyCfg.createThread)
#define OS_WakeupThreadDirect ((void (*)(void *))gProxyCfg.wakeupThreadDirect)
#define OS_Sleep            ((void (*)(u32))gProxyCfg.sleep)
#define OS_GetLockID        ((int (*)(void))gProxyCfg.getLockId)
#define OS_AllocFromHeap    ((void *(*)(int, int, u32))gProxyCfg.allocFromHeap)
#define OS_FreeToHeap       ((void (*)(int, int, void *))gProxyCfg.freeToHeap)
#define OS_MESSAGE_NOBLOCK  0
#define OS_MESSAGE_BLOCK    1

#define WM_HEAP_ID      (*(volatile int *)gProxyCfg.heapIdAddr)
#define WM_HEAP_HANDLE  (*(volatile int *)gProxyCfg.heapHandleAddr)

/* --- state ------------------------------------------------------------------ */
static const u8 fake_bssid[6] = FAKE_BSSID;
static const char fake_ssid[] = FAKE_SSID;
static volatile int joined;
static volatile int card_up;
static volatile int link_up;               /* cart reports a USB host (NET_ST_LINK_UP) */
static volatile u32 tx_len;                /* frame waiting in txbuf for the pump, 0 = none */
static volatile int status_req;            /* the pump should (start and) read the cart's status */
static int started;

#define PUMP_STACK 1024
/* Below the card task (4), above the ARM7 main thread (16), which loops on
 * OS_Halt and so never lets anything below it run. */
#define PUMP_PRIO  14
static u32 pump_thread[0x100 / 4];         /* OSThread: 0xA4 bytes in SDK 3; room for later SDKs */
static u32 pump_stack[PUMP_STACK / 4];
static u32 txbuf[(DSNET_ETH_MAX + 3) / 4];
static u32 rxbuf[DSPICO_NET_BLOCK_LARGE_BYTES / 4];

/* --- tiny helpers (no libc) ------------------------------------------------- */
static void *u8p(void *p, u32 off) { return (u8 *)p + off; }
static u16 rd16(void *p, u32 off) { return *(volatile u16 *)u8p(p, off); }
static void wr16(void *p, u32 off, u16 v) { *(volatile u16 *)u8p(p, off) = v; }
static int mac_eq(const u8 *a, const u8 *b)
{
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}
static void copy_bytes(u8 *d, const u8 *s, u32 n) { while (n--) *d++ = *s++; }
static void zero_bytes(u8 *d, u32 n) { while (n--) *d++ = 0; }

#if PROXY_DEBUG_MELONDS
static u32 tx_count, rx_count, rx_drop, tx_fail;
#define COUNT(c) ((c)++)
static char line[160];
static u32 lp;
static void putc_(char c) { if (lp < sizeof(line) - 1) line[lp++] = c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(u32 v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) putc_("0123456789abcdef"[(v >> (i * 4)) & 15]);
}
static void flush(void)
{
    line[lp] = 0;
    *(volatile u32 *)0x04FFFA18 = (u32)line;
    lp = 0;
}
/* Two threads log; a torn line is harmless in a debug build. */
#define LOG(stmts) do { puts_("[wlproxy] "); stmts; flush(); } while (0)
#else
#define COUNT(c) do { } while (0)
#define LOG(stmts) do { } while (0)
#endif

/* --- card (pump thread only) --------------------------------------------- */
static u32 irq_disable(void)
{
    u32 cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr | 0x80) : "memory");
    return cpsr;
}

static void irq_restore(u32 cpsr)
{
    __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory");
}

/* Interrupts stay off from taking the card lock to releasing it. */
static int card_begin(u32 *cpsr)
{
    *cpsr = irq_disable();
    if (dcard_trylock())
        return 1;
    irq_restore(*cpsr);
    return 0;
}

static void card_end(u32 cpsr)
{
    dcard_unlock();
    irq_restore(cpsr);
}

/* Bits 4-7 are reserved: a word with any of them set (open bus) is not a status. */
static int status_valid(u32 st) { return (st & 0xF0) == 0; }

static int card_status(u32 *st)
{
    u32 cpsr;
    if (!card_begin(&cpsr)) return 0;
    *st = dcard_status();
    card_end(cpsr);
    if (!status_valid(*st)) *st = 0;
    link_up = (*st & DSPICO_NET_ST_LINK_UP) != 0;
    return 1;
}

/* Network mode on; tried again at each status request until the cart
 * reports it running. */
static void card_start(void)
{
    u32 cpsr, st;
    if (!card_begin(&cpsr)) return;
    dcard_ctrl(DSPICO_NET_CTRL_INIT);
    dcard_ctrl(DSPICO_NET_CTRL_IRQ_OFF);
    st = dcard_status();
    card_end(cpsr);
    if (!status_valid(st)) st = 0;
    card_up = (st & DSPICO_NET_ST_ACTIVE) != 0;
    link_up = (st & DSPICO_NET_ST_LINK_UP) != 0;
    LOG(puts_("card net init, status "); puthex(st, 8));
}

static int card_send(const u32 *buf, u32 len)
{
    u32 cpsr;
    if (!card_begin(&cpsr)) return 0;
    dcard_tx(buf, (unsigned)len);
    card_end(cpsr);
    return 1;
}

static u32 card_receive(u32 block, int *big)
{
    u32 cpsr, len;
    if (!card_begin(&cpsr)) return 0;
    len = dcard_rx(rxbuf, block, big);
    card_end(cpsr);
    return len;
}

/* --- RX: card -> MA_DATA_IND -> WM ----------------------------------------- */
static void deliver(const u8 *eth, u32 len)
{
    u32 dcf_len = len - DSNET_ETH_HEADER_LEN + DSNET_SNAP_LEN;
    if (len < DSNET_ETH_HEADER_LEN || dcf_len > DSNET_DCF_MAX) { COUNT(rx_drop); return; }

    u8 *ind = OS_AllocFromHeap(WM_HEAP_ID, WM_HEAP_HANDLE, IND_HEADER_BYTES + dcf_len + 4);
    if (!ind) { COUNT(rx_drop); LOG(puts_("RX drop: WM heap full")); return; }

    zero_bytes(ind, IND_HEADER_BYTES);
    u8 dst[6], src[6];
    if (dsnet_eth_to_dcf(eth, len, dst, src, ind + IND_HEADER_BYTES, dcf_len) != (int)dcf_len) {
        OS_FreeToHeap(WM_HEAP_ID, WM_HEAP_HANDLE, ind);
        COUNT(rx_drop);
        return;
    }
    u8 *f = ind + IND_FRAME;
    wr16(ind, REQ_CODE, WL_MA_DATA_IND);
    wr16(ind, REQ_LEN, (u16)((dcf_len + RXF_DATA + 1) / 2));
    wr16(f, RXF_LENGTH, (u16)dcf_len);
    f[RXF_RATE] = 0x14;                       /* 2 Mbps, as the hardware reports it */
    f[RXF_RSSI] = FAKE_RSSI;
    copy_bytes(f + RXF_DEST, dst, 6);
    copy_bytes(f + RXF_SRC, src, 6);

    if (!OS_SendMessage((void *)gProxyCfg.fromWlQueue, ind, OS_MESSAGE_NOBLOCK)) {
        OS_FreeToHeap(WM_HEAP_ID, WM_HEAP_HANDLE, ind);
        COUNT(rx_drop);
        LOG(puts_("RX drop: WM queue full"));
        return;
    }
    COUNT(rx_count);
}

static void pump(void *arg)
{
    (void)arg;
    u32 st;
    for (;;) {
        OS_Sleep(1);
        /* While not joined, the cart is only touched when asked (at WM
         * start-up and for scans), never on a timer: the game may be saving. */
        if (status_req) {
            if (!card_up) card_start();
            else (void)card_status(&st);
            status_req = 0;
        }
        if (!joined || !card_up) {
            tx_len = 0;
            continue;
        }
        if (tx_len && card_status(&st) && (st & DSPICO_NET_ST_TX_READY) && card_send(txbuf, tx_len)) {
            COUNT(tx_count);
            tx_len = 0;
        }
        for (int n = 0; n < 4; n++) {
            if (!card_status(&st) || !DSPICO_NET_ST_RX_COUNT(st)) break;
            u32 want = DSPICO_NET_ST_RX_LEN(st);
            u32 block = want + DSPICO_NET_RX_HEADER_BYTES <= DSPICO_NET_BLOCK_SMALL_BYTES
                      ? DSPICO_NET_BLOCK_SMALL_BYTES : DSPICO_NET_BLOCK_LARGE_BYTES;
            int big = 0;
            u32 len = card_receive(block, &big);
            if (!len || big || len > block - DSPICO_NET_RX_HEADER_BYTES) break;
            const u8 *eth = (const u8 *)rxbuf + DSPICO_NET_RX_HEADER_BYTES;
            if (joined) deliver(eth, len);
        }
    }
}

/* Network mode starts at WM start-up, so the host has enumerated the cart by
 * the time the game scans (the pump sends INIT on its first status request). */
static void ensure_started(void)
{
    if (started) return;
    started = 1;
    dcard_init((u16)OS_GetLockID());
    OS_CreateThread(pump_thread, pump, 0, (u8 *)pump_stack + PUMP_STACK, PUMP_STACK, PUMP_PRIO);
    status_req = 1;
    OS_WakeupThreadDirect(pump_thread);
    LOG(puts_("pump thread started"));
}

/* Asks the pump for a fresh status (waits up to ~20 ms); the last known
 * answer stands if the cart is busy. */
static int link_is_up(void)
{
    status_req = 1;
    for (int i = 0; status_req && i < 20; i++)
        OS_Sleep(1);
    return link_up;
}

/* --- TX: MA_DATA -> Ethernet -> card --------------------------------------- */
static void send_data(u16 *req)
{
    u16 len = rd16(req, TX_LENGTH);
    const u8 *data = *(const u8 **)u8p(req, TX_DATAP);
    for (int i = 0; tx_len; i++) {             /* the pump still has the last frame */
        if (i >= 50) { COUNT(tx_fail); LOG(puts_("TX drop: card busy")); return; }
        OS_Sleep(1);
    }
    int n = dsnet_dcf_to_eth(u8p(req, TX_DEST), u8p(req, TX_SRC), data, len,
                             (u8 *)txbuf, sizeof(txbuf));
    if (n < 0) { COUNT(tx_fail); return; }          /* not SNAP: drop, report success like a radio would */
    tx_len = (u32)n;
}

/* --- the original WMSP_WlRequest, minus the flash-error check ------------- */
static u16 *forward(u16 *req)
{
    void *msg = 0;
    OS_SendMessage((void *)gProxyCfg.toWlQueue, req, OS_MESSAGE_BLOCK);
    OS_ReceiveMessage((void *)gProxyCfg.confirmQueue, &msg, OS_MESSAGE_BLOCK);
    return (u16 *)msg;
}

static int scan_wants_fake(u16 *req)
{
    const u8 *bssid = u8p(req, SCAN_BSSID);
    int bcast = 1;
    for (int i = 0; i < 6; i++)
        if (bssid[i] != 0xFF) bcast = 0;
    if (!bcast && !mac_eq(bssid, fake_bssid)) return 0;

    u16 ssidlen = rd16(req, SCAN_SSIDLEN);
    if (ssidlen) {
        if (ssidlen != sizeof(fake_ssid) - 1) return 0;
        const u8 *ssid = u8p(req, SCAN_SSID);
        for (u32 i = 0; i < ssidlen; i++)
            if (ssid[i] != (u8)fake_ssid[i]) return 0;
    }

    const u8 *ch = u8p(req, SCAN_CHLIST);
    for (int i = 0; i < 16 && ch[i]; i++)
        if (ch[i] == FAKE_CHANNEL) return 1;
    return 0;
}

/* Our BSS description: 32 halfwords (WlBssDesc without game info). */
#define FAKE_DESC_HW 32

static void write_fake_desc(u8 *d)
{
    zero_bytes(d, FAKE_DESC_HW * 2);
    wr16(d, 0, FAKE_DESC_HW);
    wr16(d, 2, FAKE_RSSI);
    copy_bytes(d + 4, fake_bssid, 6);
    wr16(d, 10, sizeof(fake_ssid) - 1);
    copy_bytes(d + 12, (const u8 *)fake_ssid, sizeof(fake_ssid) - 1);
    wr16(d, 44, 0x0001);                  /* capaInfo: ESS, no privacy */
    wr16(d, 46, 0x0003);                  /* basic rates: 1, 2 Mbps */
    wr16(d, 48, 0x0003);                  /* supported rates */
    wr16(d, 50, 100);                     /* beacon period (TU) */
    wr16(d, 52, 1);                       /* DTIM period */
    wr16(d, 54, FAKE_CHANNEL);
}

/* Appends DSPICO-NET to a scan confirm the real WL has filled in.
 * cfm: +2 length (halfwords after the header), +4 resultCode, +6 foundMap,
 * +8 bssDescCount, +10 bssDescList (each starts with its length in halfwords).
 * capHw: the capacity WM put in the length field before the request. */
static void append_fake(u8 *cfm, u32 capHw)
{
    u32 count = rd16(cfm, 8), usedHw = 3;
    u8 *d = cfm + 10;
    for (u32 i = 0; i < count; i++) {
        u32 len = rd16(d, 0);
        if (len == 0 || usedHw + len > capHw)
            return;                        /* not a list we understand: leave it */
        if (mac_eq(d + 4, fake_bssid))
            return;                        /* already there */
        usedHw += len;
        d += len * 2;
    }
    if (usedHw + FAKE_DESC_HW > capHw)
        return;                            /* no room */
    write_fake_desc(d);
    wr16(cfm, 8, (u16)(count + 1));
    wr16(cfm, 6, (u16)(rd16(cfm, 6) | (1u << FAKE_CHANNEL)));
    wr16(cfm, 4, 0);                       /* found something: success */
    wr16(cfm, 2, (u16)(usedHw + FAKE_DESC_HW));
}

/* --- entry: installed over WMSP_WlRequest -------------------------------- */
u16 *proxy_WlRequest(u16 *req)
{
    u16 code = rd16(req, REQ_CODE);
    u8 *cfm = u8p(req, REQ_BODY + rd16(req, REQ_LEN) * 2u);

    ensure_started();

    switch (code) {
    case WL_MLME_RESET:
        joined = 0;
        break;

    case WL_MLME_SCAN:
        /* The real radio always scans (access points, and other DSes for
         * local wireless); DSPICO-NET is added to its results while the cart
         * has a USB host. */
        if (scan_wants_fake(req) && link_is_up()) {
            u32 capHw = rd16(cfm, 2);
            u16 *res = forward(req);
            append_fake(cfm, capHw);
            return res;
        }
        break;

    case WL_MLME_JOIN:
        if (mac_eq(u8p(req, JOIN_BSSID), fake_bssid)) {
            wr16(cfm, 4, 0);
            wr16(cfm, 6, 0);
            copy_bytes(cfm + 8, fake_bssid, 6);
            LOG(puts_("join fake AP"));
            return req;
        }
        break;

    case WL_MLME_AUTH:
        if (mac_eq(u8p(req, PEER_MAC), fake_bssid)) {
            wr16(cfm, 4, 0);
            wr16(cfm, 6, 0);
            copy_bytes(cfm + 8, fake_bssid, 6);
            wr16(cfm, 14, rd16(req, AUTH_ALGO));
            return req;
        }
        break;

    case WL_MLME_ASS:
        if (mac_eq(u8p(req, PEER_MAC), fake_bssid)) {
            wr16(cfm, 4, 0);
            wr16(cfm, 6, 0);
            wr16(cfm, 8, 1);
            joined = 1;
            LOG(puts_("assoc fake AP, aid 1"));
            return req;
        }
        break;

    case WL_MLME_DEAUTH:
    case WL_MLME_DISASS:
        if (mac_eq(u8p(req, PEER_MAC), fake_bssid)) {
            wr16(cfm, 4, 0);
            if (code == WL_MLME_DEAUTH)
                copy_bytes(cfm + 6, fake_bssid, 6);
            joined = 0;
            LOG(puts_("leave fake AP: tx "); puthex(tx_count, 4); puts_(" rx "); puthex(rx_count, 4);
                puts_(" rxdrop "); puthex(rx_drop, 4); puts_(" txfail "); puthex(tx_fail, 4));
            return req;
        }
        break;

    case WL_MA_DATA:
        if (joined) {
            send_data(req);
            wr16(cfm, 4, 0);
            wr16(cfm, 6, 0);
            return req;
        }
        break;

    case WL_MA_CLRDATA:
        if (joined) {
            wr16(cfm, 4, 0);
            return req;
        }
        break;
    }

    return forward(req);
}
