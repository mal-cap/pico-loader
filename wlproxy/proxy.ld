/* Flat ARM7 blob, linked at 0 with relocations kept (-q); tools/proxy_blob.py
 * turns the ELF into blob + R_ARM_ABS32 offsets so a patcher can place it
 * anywhere. The entry point must be ARM code at the very start, because the
 * WMSP_WlRequest patch reaches it with `ldr pc, =proxy_WlRequest` (no interworking
 * on ARMv4T). */
ENTRY(proxy_WlRequest)
SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.text.proxy_WlRequest))
        KEEP(*(.data.proxy_cfg))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
    }
    __load_end = .;
    .bss (NOLOAD) : {
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
    }
    __bss_end = .;
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
