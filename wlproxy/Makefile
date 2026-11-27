# Wi-Fi proxy for the game's ARM7 (ARM mode, ARMv4T). Built by Makefile.arm9
# when WLPROXY=1; can also be built on its own.
#
#   make            build $(B)/proxy.elf (linked at 0, relocations kept)
#   make header     $(B)/wlproxy_blob.h, the image embedded in picoLoader9
#   make size       show what the proxy costs
#
#   B=dir           build directory (default: build)
#   CROSS=prefix    toolchain prefix (default: arm-none-eabi-)
#   EXTRA_CFLAGS    e.g. -DPROXY_DEBUG_MELONDS=1 for log output in melonDS

CROSS      ?= arm-none-eabi-
CC         := $(CROSS)gcc
SIZE       := $(CROSS)size
B          ?= build

CFLAGS := -marm -mcpu=arm7tdmi -mthumb-interwork -Os -ffreestanding -fno-builtin \
          -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections \
          -Wall -Wextra -Werror -std=gnu11 -I../arm9/source/patches/arm7/wifi $(EXTRA_CFLAGS)

SRCS := proxy.c dspico_card7.c frame.c
OBJS := $(patsubst %.c,$(B)/%.o,$(SRCS))
HDRS := proxy_cfg.h dspico_card7.h dspico_net_proto.h frame.h ../arm9/source/patches/arm7/wifi/wl_locate.h

.PHONY: all header size clean
all: $(B)/proxy.elf

$(B)/%.o: %.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/proxy.elf: $(OBJS) proxy.ld
	$(CC) $(CFLAGS) -nostdlib -Wl,-T,proxy.ld -Wl,-q \
	      -Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Map,$(B)/proxy.map $(OBJS) -lgcc -o $@

$(B):
	mkdir -p $(B)

header: $(B)/wlproxy_blob.h
$(B)/wlproxy_blob.h: $(B)/proxy.elf ../tools/proxy_blob.py
	python3 -I ../tools/proxy_blob.py $< --header $@

size: $(B)/proxy.elf
	@$(SIZE) -A $< | grep -E "text|bss"

clean:
	rm -rf $(B)
