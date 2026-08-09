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
 * **各エントリには分岐 1 命令だけを置き、本体は表の外に出す。**
 *
 * 最初は本体を表の中に直接書いていたが、**IRQ の本体が 128 バイトに
 * 収まっていなかった** (レジスタの退避と復元で 42 命令 = 168 バイト)。
 * 結果、+0x300 以降のエントリが 1 枠ずつ後ろへずれていた。objdump の実測:
 *
 *   +0x280 IRQ の本体が 0x...324 まで続く  → +0x300 (FIQ) の枠を侵食
 *   FIQ が SError の枠へ、SError が「下位 EL 同期」の枠へ
 *
 * M1 で症状が出なかったのは、**使っていた入口がタイマの IRQ 1 つだけ**
 * だったから。ずれていたのはその後ろの、まだ使っていないエントリ。
 *
 * 命令数を数えて収めるより、**収まりようがない形にする**ほうが確実。
 * 以降どのエントリを実装しても表は壊れない。
 */

/* 表のエントリ。128 バイト枠の先頭で本体へ飛ぶだけ */
.macro VENTRY, target
.align 7
    b       \target
.endm

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

/* 積んだ ELR の位置。SAVE_ALL が x30 と対で積むので、その 8 バイト先 */
#define SAVED_ELR_OFF (16 * 15 + 8)

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
.macro UNEXPECTED_BODY, name, which
\name:
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
    VENTRY  vec_unexpected_0
    VENTRY  vec_unexpected_1
    VENTRY  vec_unexpected_2
    VENTRY  vec_unexpected_3

/* --- 現在の EL、SP_ELx を使用中 (カーネル実行中) ---------------------- */
    VENTRY  vec_el1_sync        /* 同期例外。M2 の MMU 探針で使う */
    VENTRY  vec_el1_irq         /* IRQ。M1 のタイマがここ */
    VENTRY  vec_unexpected_6    /* FIQ */
    VENTRY  vec_unexpected_7    /* SError */

/* --- 下位 EL (AArch64) = EL0 から。M3a で実装した ---------------------- */
    VENTRY  vec_el0_sync        /* svc と、ユーザーが起こしたアボート */
    VENTRY  vec_el0_irq         /* **EL0 実行中のタイマはここに来る** */
    VENTRY  vec_unexpected_10   /* FIQ */
    VENTRY  vec_unexpected_11   /* SError */

/* --- 下位 EL (AArch32) から。32bit は使わない ------------------------- */
    VENTRY  vec_unexpected_12
    VENTRY  vec_unexpected_13
    VENTRY  vec_unexpected_14
    VENTRY  vec_unexpected_15

/* ---- ここから先は表の外。長さの制限は無い ---------------------------- */

/* カーネル実行中の同期例外。M1 では無条件に止めていたが、M2 で
 * 「わざと未マップの VA を読んで translation fault を確かめる」探針を
 * 入れたので、**想定内のものだけ復帰できる**ようにした。
 *
 * C 側 (aarch64_sync_exception) が 1 を返したときだけ、例外を起こした命令の
 * 次から再開する。既定は 0 = ここで止まる。想定外の同期例外を黙って
 * 読み飛ばすと原因不明の暴走になる */
vec_el1_sync:
    SAVE_ALL
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      aarch64_sync_exception
    cbz     w0, 1f
    ldr     x1, [sp, #SAVED_ELR_OFF]
    add     x1, x1, #4          /* AArch64 の命令は 4 バイト固定 */
    str     x1, [sp, #SAVED_ELR_OFF]
    RESTORE_ALL
    eret
1:
    b       .

/* **切り替えは IRQ の出口で行う** (aarch64_task_resched_if_needed)。
 * ハンドラの途中で切り替えると、まだ積んでいないものが出る。
 * ここまで来れば SAVE_ALL が済んでいるので、戻ってきたときに続きから
 * 再開できる */
vec_el1_irq:
    SAVE_ALL
    bl      aarch64_irq_handler
    bl      aarch64_task_resched_if_needed
    RESTORE_ALL
    eret

/* EL0 からの同期例外。svc (システムコール) と、ユーザーが起こしたアボートの
 * 両方がここに来る。ESR の EC で C 側が振り分ける。
 *
 * **積んだフレームの先頭を C に渡す。** システムコールは x0-x7 を引数として
 * 読み、x0 に戻り値を書き戻す必要があるので、フレームを直接触らせる。
 * 戻り先 (ELR) を書き替えれば「EL0 に戻さずカーネルへ抜ける」もできる */
vec_el0_sync:
    SAVE_ALL
    mov     x0, sp              /* フレームの先頭 = 保存した x0 の位置 */
    mrs     x1, esr_el1
    mrs     x2, far_el1
    bl      aarch64_lower_el_sync
    RESTORE_ALL
    eret

/* EL0 実行中の IRQ。**カーネル実行中の IRQ (+0x280) とは別の入口。**
 * ここを実装しないと、EL0 に降りた瞬間にタイマが取りこぼされる */
vec_el0_irq:
    SAVE_ALL
    bl      aarch64_irq_handler
    bl      aarch64_task_resched_if_needed
    RESTORE_ALL
    eret

    UNEXPECTED_BODY vec_unexpected_0,  0
    UNEXPECTED_BODY vec_unexpected_1,  1
    UNEXPECTED_BODY vec_unexpected_2,  2
    UNEXPECTED_BODY vec_unexpected_3,  3
    UNEXPECTED_BODY vec_unexpected_6,  6
    UNEXPECTED_BODY vec_unexpected_7,  7
    UNEXPECTED_BODY vec_unexpected_10, 10
    UNEXPECTED_BODY vec_unexpected_11, 11
    UNEXPECTED_BODY vec_unexpected_12, 12
    UNEXPECTED_BODY vec_unexpected_13, 13
    UNEXPECTED_BODY vec_unexpected_14, 14
    UNEXPECTED_BODY vec_unexpected_15, 15
