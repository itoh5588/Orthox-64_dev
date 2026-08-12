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

    /* **入口の EL を、降ろす前に控える。** ここから下で EL3 -> EL2 -> EL1 と
     * 降ろすので、C から CurrentEL を読んでも常に EL1 で、どの EL で
     * 飛んできたかが分からなくなる。**それが分からないと、実機で沈黙した
     * ときに原因を切り分けられない** (実際、降格後の EL1 表示を根拠に
     * 「raspi4b は EL3 で来る」と読み違えた)。bss を 0 埋めした後に
     * aarch64_entry_el へ書く */
    mrs     x22, CurrentEL
    lsr     x22, x22, #2

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

    /* ---- EL3 で起動していたら EL2 へ降りる --------------------------------
     *
     * **どの EL で来るかは機械とファームウェアで違う。決め打ちしない。**
     * 実測:
     *
     *   QEMU virt (既定)           EL1
     *   QEMU virt,virtualization=on EL2
     *   QEMU raspi3b               EL1
     *   QEMU raspi4b               EL2  (リセット直後は EL3 だが、
     *                                    QEMU の arm_boot が入口を EL2 にする。
     *                                    -d cpu_reset の PSTATE はリセット時点の
     *                                    値なので、入口の EL の根拠にならない)
     *   Raspberry Pi 4 実機        EL2  (ファームウェアの armstub8 が
     *                                    EL3 -> EL2 に降ろしてから飛ぶ)
     *
     * **実機でも config.txt に kernel_old=1 を書くと armstub を経由せず
     * EL3 のまま飛んでくる。** EL3 では EL1 のベクタも MMU 設定も効かないので
     * 沈黙する。10 数命令で塞げるので入れておく。
     *
     * 手順は Raspberry Pi 公式の armstub8.S (BSD-3-Clause,
     * raspberrypi/tools) が EL3 でやっていることに合わせた。
     * **コードは写していない。順序と、どのレジスタが要るかだけを取った。** */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.ne    6f

    /* 下位 EL は AArch64 (RW) / HVC 有効 (HCE) / SMC 無効 (SMD) /
     * 非セキュア (NS)。bit 4,5 は RES1。
     * **RW を 0 のままにすると EL2 が AArch32 になり、eret した瞬間に迷子** */
    movz    x0, #0x05b1
    msr     scr_el3, x0

    /* FP / SIMD を EL3 でトラップしない (TFP = bit 10 を 0 に) */
    msr     cptr_el3, xzr

    /* **CNTFRQ_EL0 は EL3 でしか書けない。** ここを 0 のままにすると
     * EL1 のタイマ計算が全部壊れる。armstub は BCM2711 で 54MHz を入れる。
     * **既に入っていれば触らない** — 値は機械ごとに違うので、
     * 誰かが入れてあるならそちらが正しい */
#if defined(AARCH64_CNTFRQ_HZ) && (AARCH64_CNTFRQ_HZ != 0)
    mrs     x0, cntfrq_el0
    cbnz    x0, 7f
    ldr     x0, =AARCH64_CNTFRQ_HZ
    msr     cntfrq_el0, x0
7:
#endif

    /* **全割り込みを Group 1 (非セキュア) に移す。**
     *
     * GICv2 のセキュリティ拡張では **GICD_IGROUPR はセキュア側からしか
     * 書けない**。既定は全部 Group 0 (セキュア) なので、このまま非セキュアへ
     * 降りると **EL1 は 1 本も割り込みを受け取れない**。
     * 実測 (virt,secure=on): 例外が 1 つも上がらず、タイマ待ちで止まる。
     *
     * **EL3 に居るいましか直せない。** armstub8.S の setup_gic が
     * enable_gic=1 のときにやっているのと同じ仕事で、実機で armstub を
     * 経由する構成では既に済んでいる。
     *
     * 番地はビルド時に渡す。**分からなければ何もしない** — 当てずっぽうの
     * 番地に書くほうが危ない。その場合は割り込みが来ないが、
     * 入口 EL を表示するので原因までは辿れる */
#if defined(AARCH64_EARLY_GICD) && (AARCH64_EARLY_GICD != 0)
    ldr     x1, =AARCH64_EARLY_GICD
    mov     w2, #3
    str     w2, [x1]                /* GICD_CTLR = Grp0 | Grp1 */
    add     x1, x1, #0x80           /* GICD_IGROUPR */
    mov     w2, #-1
    mov     x3, #0
8:
    str     w2, [x1, x3]
    add     x3, x3, #4
    cmp     x3, #32                 /* 8 本 = 256 割り込み */
    b.lo    8b
#endif
#if defined(AARCH64_EARLY_GICC) && (AARCH64_EARLY_GICC != 0)
    ldr     x1, =AARCH64_EARLY_GICC
    mov     w2, #0xff
    str     w2, [x1, #4]            /* セキュア側 GICC_PMR = 0xff */
    movz    w2, #0x01e7
    str     w2, [x1]                /* セキュア側 GICC_CTLR = Grp0|Grp1|AckCtl */
#endif

    /* EL2 の SCTLR は RES1 だけ立てた最小値 (0x30c50830)。MMU / キャッシュ off。
     * **ここを放っておくと EL2 が不定の設定で走る** */
    movz    x0, #0x0830
    movk    x0, #0x30c5, lsl #16
    msr     sctlr_el2, x0

    /* EL2 のスタック。いま組んだものをそのまま引き継ぐ。
     * **SP_EL2 は EL3 からしか書けない。** ここで入れないと EL2h で
     * 不定の sp を使う */
    mov     x0, sp
    msr     sp_el2, x0

    /* 戻り先の状態: EL2h / DAIF は全マスク */
    movz    x0, #0x03c9
    msr     spsr_el3, x0

    adr     x0, 6f
    msr     elr_el3, x0
    eret

6:
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
     * QEMU で再現できるので、実機が無くても検証できる。
     *
     * **CurrentEL はここで読み直す。** 上の EL3 経路から eret で来ることが
     * あるので、降りる前の値を使い回さない */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    4f                  /* 既に EL1 なら何もしない */

    /* EL1 は AArch64 (HCR_EL2.RW = 1)。ここを 0 のままにすると EL1 が
     * AArch32 扱いになり、eret した瞬間に迷子になる */
    movz    x0, #0x8000, lsl #16
    msr     hcr_el2, x0

    /* **FP / SIMD を EL2 でトラップしない。** 0x33FF は CPTR_EL2 の RES1
     * ビットだけを立てた値で、TFP (bit 10) と TCPAC (bit 31) が 0 になる。
     *
     * ここを落とさないと、**下で CPACR_EL1 を開けても EL0 の FP が EL2 に
     * トラップされる**。EL1 起動では CPTR_EL2 を触れないし関係もないので、
     * EL2 経路にだけ要る。ファームウェアがどう置いていくかは当てにしない
     * (rpi4-osdev の el2_to_el1 も同じことをしている) */
    movz    x0, #0x33ff
    msr     cptr_el2, x0

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
    /* 控えておいた入口の EL を C から読めるところへ置く。
     * **0 埋めの後でないと消される** (aarch64_entry_el は .bss) */
    ldr     x1, =aarch64_entry_el
    sub     x1, x1, x21
    str     x22, [x1]

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
