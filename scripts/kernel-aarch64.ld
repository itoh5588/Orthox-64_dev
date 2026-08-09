/* AArch64 + QEMU virt 用のリンカスクリプト。
 *
 * 配置は実機の DTB から取った値に合わせてある (推測ではない):
 *   memory@40000000  reg = 0x40000000  size = 0x20000000 (512MB)
 *
 * riscv64 が RAM 先頭 (0x80000000) + 0x200000 に置いているのと同じ流儀で
 * 0x40200000 に置く。先頭を空けるのは QEMU が DTB を RAM 先頭付近に
 * 置くため。
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40200000;
    __kernel_start = .;

    .text : ALIGN(0x1000) {
        __text_start = .;
        *(.text.start)
        *(.text .text.*)
        __text_end = .;
    }

    .rodata : ALIGN(0x1000) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data : ALIGN(0x1000) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss : ALIGN(0x1000) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(0x1000);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
    }
}
