/* カーネルのエントリーポイントを指定 */
ENTRY(_start)

/* Limine v7.x のハイヤーハーフ配置 (2MB アラインメント) */
SECTIONS
{
    /* カーネルの仮想アドレス開始位置 */
    . = 0xffffffff80000000;

    .text : {
        *(.text .text.*)
    } :text

    /* 2MB (0x200000) 境界にアライン (Limine 推奨) */
    . = ALIGN(0x200000);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x200000);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}

PHDRS
{
    text    PT_LOAD    FLAGS(5); /* R E */
    rodata  PT_LOAD    FLAGS(4); /* R   */
    data    PT_LOAD    FLAGS(6); /* RW  */
}
