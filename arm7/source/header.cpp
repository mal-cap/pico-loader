#include "common.h"
#include "header.h"

extern "C" void _start();
extern u8 __bss_start[];
extern u8 __bss_size[];

[[gnu::section(".crt0")]]
[[gnu::used]]
pload_header7_t gLoaderHeader
{
    .entryPoint = (void*)&_start,
    .apiVersion = PICO_LOADER_API_VERSION,
#ifdef PICO_LOADER_WLPROXY_DEFAULT_ON
    .v4 = { .flags = PLOAD_FLAG_WIFI_PROXY }
#endif
};