/*
 * DSpico network card commands (docs/network-mode.md in the DSpico firmware).
 * Shared by the firmware and DS-side senders, so not every definition is
 * used on both sides.
 */
#ifndef DSPICO_NET_PROTO_H
#define DSPICO_NET_PROTO_H

#define DSPICO_NET_PROTO_VERSION 1

/* Opcodes (first command byte). Free in stock firmware's unscrambled game mode. */
#define DSPICO_NET_OP_CTRL   0xEC
#define DSPICO_NET_OP_TX     0xED
#define DSPICO_NET_OP_RX     0xEE
#define DSPICO_NET_OP_STATUS 0xEF

/* NET_CTRL sub-commands (second command byte). */
#define DSPICO_NET_CTRL_INIT    0x01
#define DSPICO_NET_CTRL_DEINIT  0x02
#define DSPICO_NET_CTRL_IRQ_ON  0x03
#define DSPICO_NET_CTRL_IRQ_OFF 0x04

/* Block size class (low bit of the second command byte for TX and RX). */
#define DSPICO_NET_BLOCK_SMALL 0 /* 512 bytes  */
#define DSPICO_NET_BLOCK_LARGE 1 /* 2048 bytes */
#define DSPICO_NET_BLOCK_SMALL_BYTES 512
#define DSPICO_NET_BLOCK_LARGE_BYTES 2048

/* NET_TX chunk mode, for frames over one small block: cmd[1] has
 * DSPICO_NET_TX_CHUNK set, cmd[2] is the chunk index (0-3), cmd[3] bit 0 marks
 * the last chunk, cmd[6..7] is the whole frame's length. A single DS->cart
 * write must not cross 512 bytes; see the spec. */
#define DSPICO_NET_TX_CHUNK      0x02
#define DSPICO_NET_TX_CHUNK_LAST 0x01

/* Ethernet II without FCS. */
#define DSPICO_NET_FRAME_MIN 14
#define DSPICO_NET_FRAME_MAX 1514

/* NET_RX payload: 4-byte header, then the frame. */
#define DSPICO_NET_RX_HEADER_BYTES 4
#define DSPICO_NET_RX_SMALL_MAX (DSPICO_NET_BLOCK_SMALL_BYTES - DSPICO_NET_RX_HEADER_BYTES)
#define DSPICO_NET_RX_FLAG_TOO_BIG 0x0001 /* frame did not fit; not popped */

/* NET_STATUS word. */
#define DSPICO_NET_ST_LINK_UP     (1u << 0)
#define DSPICO_NET_ST_TX_READY    (1u << 1)
#define DSPICO_NET_ST_RX_OVERFLOW (1u << 2)
#define DSPICO_NET_ST_ACTIVE      (1u << 3)
#define DSPICO_NET_ST_RX_COUNT(w) (((w) >> 8) & 0xFFu)
#define DSPICO_NET_ST_RX_LEN(w)   (((w) >> 16) & 0xFFFFu)
#define DSPICO_NET_ST_MAKE(flags, count, len) \
    ((unsigned)(flags) | ((unsigned)((count) > 255 ? 255 : (count)) << 8) | ((unsigned)(len) << 16))

/*
 * 64-bit command builders, most significant byte first on the bus
 * (the form libtwl's card_romSetCmd takes).
 */
#define DSPICO_NET_CMD_CTRL(sub) \
    (((unsigned long long)DSPICO_NET_OP_CTRL << 56) | ((unsigned long long)(sub) << 48))
#define DSPICO_NET_CMD_TX(blockClass, length) \
    (((unsigned long long)DSPICO_NET_OP_TX << 56) | ((unsigned long long)((blockClass) & 1) << 48) | \
     ((unsigned long long)((length) & 0xFFFF)))
#define DSPICO_NET_CMD_RX(blockClass) \
    (((unsigned long long)DSPICO_NET_OP_RX << 56) | ((unsigned long long)((blockClass) & 1) << 48))
#define DSPICO_NET_CMD_STATUS \
    ((unsigned long long)DSPICO_NET_OP_STATUS << 56)

#endif
