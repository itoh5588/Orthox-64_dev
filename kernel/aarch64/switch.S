/* コンテキストスイッチ (M3c-1)。riscv64 の arch_context_switch に相当する。
 *
 *   void aarch64_context_switch(aarch64_context_t* next, aarch64_context_t* prev)
 *     x0 = 次に走らせるもの / x1 = いまの自分 (ここに退避する)
 *
 * **積むのは callee-saved だけでよい。** x0-x18 は「呼び出し側が壊されても
 * よいと承知しているレジスタ」なので、この関数を呼んだ時点で保存の義務が無い。
 * riscv64 が s0-s11 + ra + sp だけ積むのと同じ理屈。
 *
 * **戻り先 (x30) を仕込むと、そこから走り始める。** 新しく作ったタスクは
 * ここに入口の番地を入れておけば、最初の switch でそこへ「戻る」。
 *
 * sp は SP_EL1。**タスクごとに別のカーネルスタックを持たせること。**
 * EL0 実行中に割り込みが入ると CPU が SP_EL1 にフレームを積むので、
 * 共有すると別のタスクのフレームを踏む。
 */
#include "aarch64/task.h"

/* **FP/SIMD 命令をここだけ使えるようにする (P3-3)。**
 *
 * カーネルは -mgeneral-regs-only でビルドしていて、そのオプションは .S にも
 * 効く (`instruction requires: fp-armv8` で落ちることを実測した)。
 * カーネル自身が FP を使わない方針は変えたくないので、**このファイルの中だけ**
 * 解除する。ここでやるのは退避と復元だけで、演算はしない */
.arch armv8-a+fp+simd

.section .text
.globl aarch64_context_switch
aarch64_context_switch:
    /* いまの自分を x1 が指す枠へ退避 */
    stp     x19, x20, [x1, #AARCH64_CTX_X19]
    stp     x21, x22, [x1, #AARCH64_CTX_X21]
    stp     x23, x24, [x1, #AARCH64_CTX_X23]
    stp     x25, x26, [x1, #AARCH64_CTX_X25]
    stp     x27, x28, [x1, #AARCH64_CTX_X27]
    stp     x29, x30, [x1, #AARCH64_CTX_X29]
    mov     x9, sp
    str     x9, [x1, #AARCH64_CTX_SP]
    /* **割り込みの開け閉めもタスクごと。** 復元しないと、idle に
     * 閉じられたまま起こされて、以後どの割り込みも受け取れなくなる */
    mrs     x9, daif
    str     x9, [x1, #AARCH64_CTX_DAIF]

    /* **FP/SIMD も退避する (P3-3)。**
     *
     * ユーザーは FP を使う (musl の memcpy が NEON を踏む)。保存しないと
     * 2 本以上が並んだ瞬間に互いの v0-v31 を踏み合い、**落ちずに計算結果だけ
     * 壊れる。** 呼び出し規約では v8-v15 の下半分だけが callee-saved だが、
     * ここは「別のプロセスへ移る」場所なので **32 本すべてを持ち越す。**
     *
     * ここを通らない切り替え経路を作らないこと。新しいタスクも下の復元側を
     * 通って走り出すので、init_fp_state が 0 にした状態から始まる */
    add     x9, x1, #AARCH64_CTX_V0
    stp     q0,  q1,  [x9, #16 * 0]
    stp     q2,  q3,  [x9, #16 * 2]
    stp     q4,  q5,  [x9, #16 * 4]
    stp     q6,  q7,  [x9, #16 * 6]
    stp     q8,  q9,  [x9, #16 * 8]
    stp     q10, q11, [x9, #16 * 10]
    stp     q12, q13, [x9, #16 * 12]
    stp     q14, q15, [x9, #16 * 14]
    stp     q16, q17, [x9, #16 * 16]
    stp     q18, q19, [x9, #16 * 18]
    stp     q20, q21, [x9, #16 * 20]
    stp     q22, q23, [x9, #16 * 22]
    stp     q24, q25, [x9, #16 * 24]
    stp     q26, q27, [x9, #16 * 26]
    stp     q28, q29, [x9, #16 * 28]
    stp     q30, q31, [x9, #16 * 30]
    /* **制御・状態レジスタも持ち越す。** 丸めモードや例外フラグを
     * 引き継がないと、同じ入力で違う結果を出すタスクができる */
    mrs     x9, fpcr
    str     x9, [x1, #AARCH64_CTX_FPCR]
    mrs     x9, fpsr
    str     x9, [x1, #AARCH64_CTX_FPSR]

    /* **TLS ベースも退避する。**
     *
     * TPIDR_EL0 は **EL0 から読み書きできる**ので musl が自分で設定する。
     * ユーザーが所有しカーネルが持ち越すべきレジスタで、FP/SIMD と同じ性格。
     *
     * 退避しないと最後に走ったタスクの値が残る。**fork した親子は同じ像なので
     * 値も同じで症状が出ない** — 子が exec して別の像になった瞬間に、親が
     * 他人の TLS を指して musl の exit の中で落ちる (ldur w8, [tpidr_el0, #-168]
     * で pthread_self()->tid を読むため)。
     *
     * ここを ORTHOX_NO_TLS_SWITCH で潰せるようにしてあるのは **逆確認のため**。
     * 検査が本当に火を噴くことを確かめずに信じない (日報2026-08-10 追§5) */
#ifndef ORTHOX_NO_TLS_SWITCH
    mrs     x9, tpidr_el0
    str     x9, [x1, #AARCH64_CTX_TPIDR]
#endif

    /* 次のものを x0 が指す枠から復元 */
    ldp     x19, x20, [x0, #AARCH64_CTX_X19]
    ldp     x21, x22, [x0, #AARCH64_CTX_X21]
    ldp     x23, x24, [x0, #AARCH64_CTX_X23]
    ldp     x25, x26, [x0, #AARCH64_CTX_X25]
    ldp     x27, x28, [x0, #AARCH64_CTX_X27]
    ldp     x29, x30, [x0, #AARCH64_CTX_X29]
    ldr     x9, [x0, #AARCH64_CTX_SP]
    mov     sp, x9
    ldr     x9, [x0, #AARCH64_CTX_DAIF]
    msr     daif, x9

    /* FP/SIMD の復元。**daif より後、ret より前。**
     * 順序に依存は無いが、退避と対称に並べて読めるようにしておく */
    ldr     x9, [x0, #AARCH64_CTX_FPCR]
    msr     fpcr, x9
    ldr     x9, [x0, #AARCH64_CTX_FPSR]
    msr     fpsr, x9
    add     x9, x0, #AARCH64_CTX_V0
    ldp     q0,  q1,  [x9, #16 * 0]
    ldp     q2,  q3,  [x9, #16 * 2]
    ldp     q4,  q5,  [x9, #16 * 4]
    ldp     q6,  q7,  [x9, #16 * 6]
    ldp     q8,  q9,  [x9, #16 * 8]
    ldp     q10, q11, [x9, #16 * 10]
    ldp     q12, q13, [x9, #16 * 12]
    ldp     q14, q15, [x9, #16 * 14]
    ldp     q16, q17, [x9, #16 * 16]
    ldp     q18, q19, [x9, #16 * 18]
    ldp     q20, q21, [x9, #16 * 20]
    ldp     q22, q23, [x9, #16 * 22]
    ldp     q24, q25, [x9, #16 * 24]
    ldp     q26, q27, [x9, #16 * 26]
    ldp     q28, q29, [x9, #16 * 28]
    ldp     q30, q31, [x9, #16 * 30]

    /* TLS ベースの復元。退避と対称に置く */
#ifndef ORTHOX_NO_TLS_SWITCH
    ldr     x9, [x0, #AARCH64_CTX_TPIDR]
    msr     tpidr_el0, x9
#endif

    /* x30 に入っている場所へ「戻る」。新しいタスクなら入口へ飛ぶ */
    ret
