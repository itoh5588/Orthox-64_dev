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

    /* x30 に入っている場所へ「戻る」。新しいタスクなら入口へ飛ぶ */
    ret
