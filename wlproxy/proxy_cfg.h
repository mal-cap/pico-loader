/* Proxy configuration: compile-time fake-AP settings, and the per-game
 * addresses WlProxyPatch writes into gProxyCfg (found by wl_locate). */
#ifndef PROXY_CFG_H
#define PROXY_CFG_H

#include "wl_locate.h"

extern wl_locate_result_t gProxyCfg;

#define FAKE_BSSID   { 0x02, 0xD5, 0x91, 0xC0, 0x00, 0x01 }   /* locally administered */
#define FAKE_SSID    "DSPICO-NET"
#define FAKE_CHANNEL 6
#define FAKE_RSSI    0x30

#ifndef PROXY_DEBUG_MELONDS
#define PROXY_DEBUG_MELONDS 0   /* 1: log to melonDS through 0x04FFFA18 */
#endif

#endif
