/* aarch64 のユーザープログラム。**カーネルの配置とは無関係の下位 VA**。
 * TTBR0 はユーザー専用なので、下位側のどこでもよい。
 *
 * **区画ごとにページ境界で分けること。** 分けないと .text (R+X) と
 * .rodata (R) が同じ 4KB ページに入り、kernel/elf.c が同じページを
 * 2 回張って**後から張ったほうの権限で上書きする**。結果、実行権が
 * 消えて EL0 で命令アボートになる (実測: ESR=0x8200000f、
 * IFSC=permission fault level 3)。
 */
ENTRY(_start)
SECTIONS
{
    . = 0x0000000000400000;
    .text : { *(.text*) }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) }
    . = ALIGN(0x1000);
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
