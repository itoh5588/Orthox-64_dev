/* **段を境目で整列させない**リンカ台本 (user/x86_straddle_probe.c 専用)。
 *
 * 普通の配置は PT_LOAD ごとに 1 ページ空けるので、段が同じページを跨ぐ道が
 * 一度も通らない。ここでは .text (R+X) の直後に .data (R+W) を置いて
 * **境目のページを 2 つの段に覆わせる。**
 *
 * .rodata を .text の前に置いてあるのは、**境目のページの実行する側に
 * 確実にコードを置く**ため。後ろに置くと境目が .rodata と .data になり、
 * コードが共有ページに乗らない。 */
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS {
    . = 0x400000;

    .rodata : { *(.rodata) *(.rodata.*) } :text
    .text   : { *(.text) *(.text.*) *(.text.tail) } :text

    /* ここで整列させない = 跨ぐ */
    .data   : { *(.data.head) *(.data) *(.data.*) } :data
    .bss    : { *(.bss) *(.bss.*) *(COMMON) } :data

    /DISCARD/ : { *(.comment) *(.note.*)  }
}
