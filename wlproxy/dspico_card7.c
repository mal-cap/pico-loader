/* See dspico_card7.h. */
#include "dspico_card7.h"
#include "dspico_net_proto.h"

#define REG8(a)  (*(volatile uint8_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))

#define REG_EXMEMSTAT   0x04000204   /* ARM7 view; bit 11 = slot owned by ARM7 */
#define REG_AUXSPICNT_H 0x040001A1
#define REG_ROMCTRL     0x040001A4
#define REG_CARDCMD     0x040001A8
#define REG_CARDDATA    0x04100010

#define ROMCTRL_BUSY    0x80000000u
#define ROMCTRL_DRQ     0x00800000u

/* Pico Loader DSpico control words; block-size field is bits 24-26. */
#define CTRL_NODATA     0xA0406000u
#define CTRL_READ4      0xA7446000u
#define CTRL_READ(bs)   (0xA0444000u | ((uint32_t)(bs) << 24))
#define CTRL_WRITE(bs)  (0xE0486000u | ((uint32_t)(bs) << 24))
#define BS_512          1
#define BS_2048         3

#define CARD_LOCK_WORD  ((volatile uint32_t *)0x027FFFE0)   /* HW_CARD_LOCK_BUF */
#define CARD_LOCK_OWNER ((volatile uint16_t *)0x027FFFE4)

static uint16_t sLockId;

static uint32_t swp(uint32_t v, volatile uint32_t *p)
{
    uint32_t old;
    __asm__ volatile("swp %0, %1, [%2]" : "=&r"(old) : "r"(v), "r"(p) : "memory");
    return old;
}

void dcard_init(uint16_t lockId) { sLockId = lockId; }

int dcard_trylock(void)
{
    if (swp(sLockId, CARD_LOCK_WORD) != 0)
        return 0;                       /* someone (or our other thread) holds it */
    *CARD_LOCK_OWNER = sLockId;
    if (!(REG16(REG_EXMEMSTAT) & (1u << 11)) ||    /* the ARM9 still maps the slot */
        (REG32(REG_ROMCTRL) & ROMCTRL_BUSY)) {      /* someone else's transfer */
        dcard_unlock();
        return 0;
    }
    return 1;
}

void dcard_unlock(void)
{
    *CARD_LOCK_OWNER = 0;
    *CARD_LOCK_WORD = 0;
}

/* Every wait is bounded: callers may have interrupts off, and a command the
 * cart never finishes must not freeze the ARM7. A transfer takes well under
 * a millisecond; this limit is roughly 100 ms. */
#define SPIN_LIMIT 400000u

static void wait_idle(void)
{
    for (uint32_t i = 0; (REG32(REG_ROMCTRL) & ROMCTRL_BUSY) && i < SPIN_LIMIT; i++) { }
}

static void set_cmd(const uint8_t cmd[8])
{
    wait_idle();
    REG8(REG_AUXSPICNT_H) = 0x80;       /* enable, ROM mode, no IRQ */
    for (int i = 0; i < 8; i++)
        REG8(REG_CARDCMD + i) = cmd[i];
}

static void read_words(uint32_t ctrl, uint32_t *dst, unsigned words)
{
    REG32(REG_ROMCTRL) = ctrl;
    unsigned n = 0;
    for (uint32_t i = 0; (REG32(REG_ROMCTRL) & ROMCTRL_BUSY) && i < SPIN_LIMIT; i++) {
        if (REG32(REG_ROMCTRL) & ROMCTRL_DRQ) {
            uint32_t w = REG32(REG_CARDDATA);
            if (n < words) dst[n] = w;
            n++;
        }
    }
}

void dcard_ctrl(uint8_t sub)
{
    uint8_t cmd[8] = { DSPICO_NET_OP_CTRL, sub, 0, 0, 0, 0, 0, 0 };
    set_cmd(cmd);
    REG32(REG_ROMCTRL) = CTRL_NODATA;
    wait_idle();
}

uint32_t dcard_status(void)
{
    uint8_t cmd[8] = { DSPICO_NET_OP_STATUS, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t w = 0;
    set_cmd(cmd);
    read_words(CTRL_READ4, &w, 1);
    return w;
}

unsigned dcard_rx(uint32_t *buf, unsigned block, int *tooBig)
{
    int large = block > DSPICO_NET_BLOCK_SMALL_BYTES;
    uint8_t cmd[8] = { DSPICO_NET_OP_RX, (uint8_t)large, 0, 0, 0, 0, 0, 0 };
    set_cmd(cmd);
    buf[0] = 0;                         /* reads as "no frame" if the transfer times out */
    read_words(CTRL_READ(large ? BS_2048 : BS_512), buf, block / 4);
    uint32_t hdr = buf[0];
    if (tooBig) *tooBig = (hdr >> 16) & DSPICO_NET_RX_FLAG_TOO_BIG;
    return hdr & 0xFFFF;
}

static void write_block(const uint8_t cmd[8], const uint32_t *buf, unsigned have)
{
    set_cmd(cmd);
    REG32(REG_ROMCTRL) = CTRL_WRITE(BS_512);
    unsigned n = 0;
    for (uint32_t i = 0; (REG32(REG_ROMCTRL) & ROMCTRL_BUSY) && i < SPIN_LIMIT; i++) {
        if (REG32(REG_ROMCTRL) & ROMCTRL_DRQ) {
            REG32(REG_CARDDATA) = n < have ? buf[n] : 0;
            n++;
        }
    }
}

/* Always 512-byte writes: a DS->cart write crossing a 512-byte block boundary
 * gets 8 bytes replayed on real hardware. Longer frames go as chunks. */
void dcard_tx(const uint32_t *buf, unsigned len)
{
    unsigned words = (len + 3) / 4;
    if (len <= DSPICO_NET_BLOCK_SMALL_BYTES) {
        uint8_t cmd[8] = { DSPICO_NET_OP_TX, DSPICO_NET_BLOCK_SMALL, 0, 0, 0, 0,
                           (uint8_t)(len >> 8), (uint8_t)len };
        write_block(cmd, buf, words);
        return;
    }
    unsigned chunks = (len + DSPICO_NET_BLOCK_SMALL_BYTES - 1) / DSPICO_NET_BLOCK_SMALL_BYTES;
    for (unsigned i = 0; i < chunks; i++) {
        unsigned first = i * (DSPICO_NET_BLOCK_SMALL_BYTES / 4);
        uint8_t cmd[8] = { DSPICO_NET_OP_TX, DSPICO_NET_TX_CHUNK, (uint8_t)i,
                           (uint8_t)(i + 1 == chunks ? DSPICO_NET_TX_CHUNK_LAST : 0), 0, 0,
                           (uint8_t)(len >> 8), (uint8_t)len };
        write_block(cmd, buf + first, words - first);
    }
}
