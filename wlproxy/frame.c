#include "frame.h"

static void copy(uint8_t *d, const uint8_t *s, unsigned n)
{
    while (n--)
        *d++ = *s++;
}

int dsnet_dcf_to_eth(const uint8_t dst[6], const uint8_t src[6],
                     const uint8_t *dcf, unsigned dcfLen,
                     uint8_t *eth, unsigned ethCap)
{
    unsigned payloadLen, total;

    if (dcfLen < DSNET_SNAP_LEN || dcfLen > DSNET_DCF_MAX)
        return -1;
    /* LLC: DSAP AA, SSAP AA, control 03. SNAP OUI 00 00 00. */
    if (dcf[0] != 0xAA || dcf[1] != 0xAA || dcf[2] != 0x03)
        return -1;
    if (dcf[3] != 0x00 || dcf[4] != 0x00 || dcf[5] != 0x00)
        return -1;

    payloadLen = dcfLen - DSNET_SNAP_LEN;
    total = DSNET_ETH_HEADER_LEN + payloadLen;
    if (total > ethCap || total > DSNET_ETH_MAX)
        return -1;

    copy(eth, dst, 6);
    copy(eth + 6, src, 6);
    eth[12] = dcf[6];
    eth[13] = dcf[7];
    copy(eth + DSNET_ETH_HEADER_LEN, dcf + DSNET_SNAP_LEN, payloadLen);
    return (int)total;
}

int dsnet_eth_to_dcf(const uint8_t *eth, unsigned ethLen,
                     uint8_t dst[6], uint8_t src[6],
                     uint8_t *dcf, unsigned dcfCap)
{
    unsigned payloadLen, total;
    unsigned etherType;

    if (ethLen < DSNET_ETH_HEADER_LEN || ethLen > DSNET_ETH_MAX)
        return -1;
    etherType = ((unsigned)eth[12] << 8) | eth[13];
    if (etherType < 0x0600) /* 802.3 length field, not an EtherType */
        return -1;

    payloadLen = ethLen - DSNET_ETH_HEADER_LEN;
    total = DSNET_SNAP_LEN + payloadLen;
    if (total > dcfCap || total > DSNET_DCF_MAX)
        return -1;

    copy(dst, eth, 6);
    copy(src, eth + 6, 6);
    dcf[0] = 0xAA;
    dcf[1] = 0xAA;
    dcf[2] = 0x03;
    dcf[3] = 0x00;
    dcf[4] = 0x00;
    dcf[5] = 0x00;
    dcf[6] = eth[12];
    dcf[7] = eth[13];
    copy(dcf + DSNET_SNAP_LEN, eth + DSNET_ETH_HEADER_LEN, payloadLen);
    return (int)total;
}
