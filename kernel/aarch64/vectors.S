/* AArch64 の例外ベクタ表 (EL1)。
 *
 * riscv64 は stvec に「入口 1 つ」を登録し、scause を読んで自分で振り分けた。
 * AArch64 は **16 個の入口が並んだ表**を VBAR_EL1 に登録し、CPU が原因別に
 * 飛び分ける。各エントリは 128 バイト (32 命令) 固定で、表全体は 2048 バイト
 * 境界に置く必要がある。
 *
 *   +0x000  現在の EL / SP_EL0    同期 / IRQ / FIQ / SError
 *   +0x200  現在の EL / SP_ELx    同期 / IRQ / FIQ / SError   ← カーネル実行中
 *   +0x400  下位 EL (AArch64)     同期 / IRQ / FIQ / SError   ← ユーザー実行中
 *   +0x600  下位 EL (AArch32)     未使用
 *
 * M1 で要るのは「カーネル実行中の IRQ」(+0x280) だけ。残りは原因を表示して
 * 止める。**黙って無視しないこと** — 取りこぼすと原因不明のハングになる。
 */

/* 割り込みハンドラは C を呼ぶので、C が壊してよいレジスタ (x0-x18, x30) を
 * すべて退避する。加えて戻り先 (ELR_EL1) と状態 (SPSR_EL1) も積む。
 * 34 本 x 8 バイト = 272 バイト (16 バイト境界を保つ) */
.macro SAVE_ALL
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mrs     x0, elr_el1
    stp     x30, x0,  [sp, #16 * 15]
    mrs     x1, spsr_el1
    str     x1, [sp, #16 * 16]
.endm

.macro RESTORE_ALL
    ldr     x1, [sp, #16 * 16]
    msr     spsr_el1, x1
    ldp     x30, x0,  [sp, #16 * 15]
    msr     elr_el1, x0
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #272
.endm

/* 想定していない例外。原因を持って C に渡し、そこで止める。
 * x0 = 表のどのエントリか (見分けがつくように番号を渡す) */
.macro UNEXPECTED, which
    SAVE_ALL
    mov     x0, #\which
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    bl      aarch64_unexpected_exception
    b       .
.endm

.section .text
.align 11                       /* 2048 バイト境界。VBAR_EL1 の要求 */
.globl aarch64_vectors
aarch64_vectors:

/* --- 現在の EL、SP_EL0 を使用中 --------------------------------------- */
.align 7
    UNEXPECTED 0
.align 7
    UNEXPECTED 1
.align 7
    UNEXPECTED 2
.align 7
    UNEXPECTED 3

/* --- 現在の EL、SP_ELx を使用中 (カーネル実行中) ---------------------- */
.align 7
    UNEXPECTED 4                /* 同期例外 */
.align 7
    /* IRQ。M1 で使うのはここだけ */
    SAVE_ALL
    bl      aarch64_irq_handler
    RESTORE_ALL
    eret
.align 7
    UNEXPECTED 6                /* FIQ */
.align 7
    UNEXPECTED 7                /* SError */

/* --- 下位 EL (AArch64) から。M3 でユーザーが来たら使う ---------------- */
.align 7
    UNEXPECTED 8
.align 7
    UNEXPECTED 9
.align 7
    UNEXPECTED 10
.align 7
    UNEXPECTED 11

/* --- 下位 EL (AArch32) から。32bit は使わない ------------------------- */
.align 7
    UNEXPECTED 12
.align 7
    UNEXPECTED 13
.align 7
    UNEXPECTED 14
.align 7
    UNEXPECTED 15
