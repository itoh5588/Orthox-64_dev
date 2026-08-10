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

#include "aarch64/vm.h"

.section .text.start
.globl _start
.extern aarch64_early_main

_start:
    /* QEMU は x0 に DTB の物理アドレスを渡す。真っ先に退避すること。
     * ここを潰すと DTB を二度と拾えない (最初 mpidr を x0 に読んで壊した) */
    mov     x19, x0

    /* **上位 VA でリンクされたシンボルを物理に直すための差。**
     * .boot は VA = PA、それ以外は VA = PA + この値でリンクされている */
    ldr     x21, =AARCH64_KERNEL_VA_OFFSET

    /* Aff0 (CPU 番号) を取り出す。virt は Aff0 に 0,1,2... を入れる */
    mrs     x20, mpidr_el1
    and     x20, x20, #0xff
    cmp     x20, #8
    b.hs    2f

    /* ブートスタックはカーネルイメージの直後の RAM に置く。CPU ごとに 64KB。
     *
     * .bss (上位 VA) に置くと .boot から届かず、.boot に置くと nobits を
     * progbits の中に混ぜることになってイメージが 512KB 太る。
     * イメージの外を使えばどちらも避けられる。
     * MMU を入れた後は同じ物理が TTBR1 側にも張られるので使い続けられる */
    ldr     x1, =__kernel_end
    sub     x1, x1, x21         /* 物理に直す */
    mov     x2, #65536
    add     x3, x20, #1
    madd    x1, x3, x2, x1      /* base + (cpu + 1) * 64KB = この CPU の頂点 */
    mov     sp, x1

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
    /* ---- FP / SIMD を EL0 に開ける ---------------------------------------
     *
     * **CPACR_EL1.FPEN を開かないと EL0 の FP / NEON 命令が全部トラップされる。**
     * カーネル自身は -mgeneral-regs-only で FP を使わないので、ここが 0 の
     * ままでも M0-M4 は全部通る。**踏むのは libc を積んだ最初のユーザー
     * プログラムから** — musl は memcpy などで NEON を使う。
     *
     *   ESR=0x1fe00000  EC=0x07 (FP アクセスが CPACR_EL1.FPEN でトラップ)
     *
     * riscv64 の sstatus.FS に相当する。EL1 起動と EL2 降格の合流点なので、
     * どちらの経路で来てもここを通る。
     * FPEN = 0b11: EL0 / EL1 のどちらからも trap しない */
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    /* ---- ここから先は「上位 VA でリンクされたもの」を触る ---------------
     *
     * **.boot からは PC 相対で届かない** (リンク上の距離が 2^40 ある)。
     * 絶対アドレス (= 上位 VA) をリテラルプールから取り、
     * AARCH64_KERNEL_VA_OFFSET (x21 に入れてある) を引いて物理に直す。
     * 物理の並びは上位 VA の並びと同じ差でずれているので、この引き算で合う */
    /* .bss を 0 で埋める。C に入る前に済ませること */
    ldr     x1, =__bss_start
    sub     x1, x1, x21
    ldr     x2, =__bss_end
    sub     x2, x2, x21
1:
    cmp     x1, x2
    b.hs    3f
    str     xzr, [x1], #8
    b       1b
3:
    /* MMU はまだ off。**C も物理アドレスで走る。**
     * 上位 VA でリンクされた C どうしの参照は adrp (PC 相対) なので、
     * PC が物理なら結果も物理になり、そのまま動く */
    ldr     x2, =aarch64_early_main
    sub     x2, x2, x21
    mov     x0, x19             /* DTB の物理アドレスを第 1 引数で渡す */
    blr     x2

2:
    /* CPU 0 以外と、C から戻ってきたとき。**ここは .boot の中で完結させる。**
     * 上位側の aarch64_wait_forever を bl で呼ぶと届かない */
    wfe
    b       2b
