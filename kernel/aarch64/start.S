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

    /* ---- EL2 で起動していたら EL1 へ降りる --------------------------------
     *
     * QEMU virt は既定 (virtualization=off) だと EL1 から始まるが、
     * **Raspberry Pi 4 のファームウェアは EL2 で起動する**。
     *
     * EL2 のままだと例外は VBAR_EL2 を見るので、VBAR_EL1 に登録した表が
     * 使われず、タイマ割り込みを取りこぼす。実測:
     *
     *   -machine virt,virtualization=on  → CurrentEL: EL2、ticks: 0 (timer-BAD)
     *
     * QEMU で再現できるので、実機が無くても検証できる。 */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    4f                  /* 既に EL1 なら何もしない */

    /* EL1 は AArch64 (HCR_EL2.RW = 1)。ここを 0 のままにすると EL1 が
     * AArch32 扱いになり、eret した瞬間に迷子になる */
    movz    x0, #0x8000, lsl #16
    msr     hcr_el2, x0

    /* SCTLR_EL1 は RES1 ビットを立てた最小値。MMU とキャッシュは off
     * (0x30d00800 = bit 11,20,22,23,28,29)。M2 で MMU を入れるときに触る */
    movz    x0, #0x0800
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0

    /* EL1 から物理カウンタ / 物理タイマを触れるようにする。
     * ここを開けないと CNTP_* が trap して generic timer が動かない */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3          /* EL1PCTEN | EL1PCEN */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr    /* 仮想カウンタのオフセットを 0 に */

    /* 戻り先の状態: EL1h (SP_EL1 を使う) / DAIF は全マスク。
     * 割り込みは C 側が daifclr で開ける */
    movz    x0, #0x03c5
    msr     spsr_el2, x0

    /* EL1 での sp は SP_EL1。いま組んだスタックをそのまま引き継ぐ */
    mov     x0, sp
    msr     sp_el1, x0

    adr     x0, 4f
    msr     elr_el2, x0
    eret

4:
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
