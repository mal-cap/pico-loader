/*
 * Conversion between the payload a DS game hands to WM_SetDCFData
 * (LLC/SNAP header + packet, MACs carried separately) and Ethernet II.
 * Portable C, no dependencies.
 */
#ifndef DSNET_FRAME_H
#define DSNET_FRAME_H

#include <stdint.h>

#define DSNET_SNAP_LEN 8
#define DSNET_ETH_HEADER_LEN 14
#define DSNET_DCF_MAX 1508 /* WM_DCF_MAX_SIZE */
#define DSNET_ETH_MAX 1514

/*
 * DCF payload -> Ethernet II frame.
 * Returns the frame length, or -1 if the payload is not SNAP-encapsulated
 * or does not fit.
 */
int dsnet_dcf_to_eth(const uint8_t dst[6], const uint8_t src[6],
                     const uint8_t *dcf, unsigned dcfLen,
                     uint8_t *eth, unsigned ethCap);

/*
 * Ethernet II frame -> DCF payload, plus the two MACs.
 * Returns the payload length, or -1 if the frame is too short, is an
 * 802.3 length frame, or does not fit.
 */
int dsnet_eth_to_dcf(const uint8_t *eth, unsigned ethLen,
                     uint8_t dst[6], uint8_t src[6],
                     uint8_t *dcf, unsigned dcfCap);

#endif
