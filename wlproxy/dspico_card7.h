/*
 * DSpico network card commands from the ARM7, polling only (no DMA, no IRQ).
 * Freestanding; ARM mode (uses SWP).
 *
 * Bus ownership: the SDK hands the slot to the ARM7 whenever the ARM9 releases
 * the card lock (OS_UnlockCard -> MIi_SetCardProcessor(ARM7)). We take the same
 * shared lock word, then check EXMEMSTAT bit 11 before touching the bus.
 * The lock alone is not enough: Pico Loader's save code (in the ARM7 card
 * task) uses the bus without it, so callers in a game must also make sure
 * that code can't be interrupted, and can't interrupt them, mid-transfer.
 *
 * Control words are Pico Loader's DSpico values (KEY2 command bits as the
 * DSpico firmware expects in game mode).
 */
#ifndef DSPICO_CARD7_H
#define DSPICO_CARD7_H

#include <stdint.h>

/* Must be called once with an ARM7 lock ID from OS_GetLockID. */
void dcard_init(uint16_t lockId);

/* 1 = we hold the card lock, the slot is mapped to the ARM7 and no transfer
 * is in progress. */
int dcard_trylock(void);
void dcard_unlock(void);

/* The calls below require the lock. */
void dcard_ctrl(uint8_t sub);
uint32_t dcard_status(void);
/* Reads one NET_RX block into buf (4-byte aligned, 512 or 2048 bytes).
 * Returns the frame length from the block header (0 = none). *tooBig is set
 * if the frame did not fit and was left queued. */
unsigned dcard_rx(uint32_t *buf, unsigned block, int *tooBig);
/* Sends one Ethernet frame (buf 4-byte aligned, readable up to the block size). */
void dcard_tx(const uint32_t *buf, unsigned len);

#endif
