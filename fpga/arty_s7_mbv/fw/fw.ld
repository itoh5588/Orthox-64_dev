/* Boot BRAM: 64 KiB at 0x0. */
ENTRY(_start)
MEMORY { bram (rwx) : ORIGIN = 0x0, LENGTH = 64K }
SECTIONS
{
    .text   : { *(.text.start) *(.text .text.*) } > bram
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } > bram
    .data   : { *(.data .data.* .sdata .sdata.*) } > bram
    .bss    : { *(.bss .bss.* .sbss .sbss.* COMMON) } > bram
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame) *(.riscv.attributes) }
}
