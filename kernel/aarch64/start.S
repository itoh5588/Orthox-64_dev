/* AArch64 の起動入口 (QEMU virt)。
 *
 * QEMU は -kernel に渡した ELF を物理アドレスどおりに置き、全 CPU を
 * _start から走らせる。riscv64 が hart id で振り分けるのと同じで、
 * ここでは MPIDR_EL1 の Aff0 を見て 0 番以外は待たせる。
 *
 * 実行レベルは決め打ちにしない。QEMU virt は既定 (virtualization=off) だと
 * EL1 で始まるが、virtualization=on だと EL2 で始まる。**どちらで来たかは
 * 早期に知りたい情報**なので、CurrentEL は C 側で読んで表示する。
 * EL2 からの降格は M1 (例外ベクタ) と一緒に入れる。
 */

.section .bss
.align 16
aarch64_boot_stacks:
    .skip (65536 * 8)
aarch64_boot_stacks_top:

.section .text.start
.globl _start
.extern aarch64_early_main
.extern aarch64_wait_forever

_start:
    /* QEMU は x0 に DTB の物理アドレスを渡す。真っ先に退避すること。
     * ここを潰すと DTB を二度と拾えない (最初 mpidr を x0 に読んで壊した) */
    mov     x19, x0

    /* Aff0 (CPU 番号) を取り出す。virt は Aff0 に 0,1,2... を入れる */
    mrs     x20, mpidr_el1
    and     x20, x20, #0xff
    cmp     x20, #8
    b.hs    2f

    /* スタックは CPU ごとに 64KB。x20 番目の先頭 + 64KB が sp */
    adrp    x1, aarch64_boot_stacks
    add     x1, x1, :lo12:aarch64_boot_stacks
    mov     x2, #65536
    madd    x1, x20, x2, x1
    add     sp, x1, x2

    /* 起こすのは CPU 0 だけ。他は M1 まで寝かせる */
    cbnz    x20, 2f

    /* .bss を 0 で埋める。C に入る前に済ませること */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
1:
    cmp     x1, x2
    b.hs    3f
    str     xzr, [x1], #8
    b       1b
3:
    mov     x0, x19             /* DTB の物理アドレスを第 1 引数で渡す */
    bl      aarch64_early_main

2:
    b       aarch64_wait_forever

.globl aarch64_wait_forever
aarch64_wait_forever:
    wfe
    b       aarch64_wait_forever
