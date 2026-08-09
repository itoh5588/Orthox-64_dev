#ifndef ORTHOX_ARCH_AARCH64_TASK_H
#define ORTHOX_ARCH_AARCH64_TASK_H

/* .S からも読むので、**offset の #define は __ASSEMBLER__ の外**に置く。
 * C の宣言 (stdint.h を含む) は中に入れること */

/* コンテキストスイッチ (M3c-1)。
 *
 * 積むのは **AAPCS で「呼び出し先が保存する」レジスタ**だけでよい。
 * 呼び出し側が壊してよいレジスタ (x0-x18) は、switch を呼んだ時点で
 * どうなってもよいことになっているので保存しなくていい。
 * riscv64 の arch_context_switch が s0-s11 + ra + sp だけ積むのと同じ理屈。
 *
 *   x19-x28   callee-saved
 *   x29 (fp)  フレームポインタ
 *   x30 (lr)  戻り先。**ここに次に走る場所を仕込むと、そこから始まる**
 *   sp        SP_EL1。**タスクごとに別のカーネルスタック**
 *
 * 浮動小数点 (d8-d15) は積んでいない。-mgeneral-regs-only でビルドして
 * いるのでカーネルは FP を使わない。EL0 で使い始めたら足すこと。
 */
#define AARCH64_CTX_X19  0
#define AARCH64_CTX_X21  16
#define AARCH64_CTX_X23  32
#define AARCH64_CTX_X25  48
#define AARCH64_CTX_X27  64
#define AARCH64_CTX_X29  80
#define AARCH64_CTX_SP   96
#define AARCH64_CTX_SIZE 112

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct aarch64_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t x29;       /* fp */
    uint64_t x30;       /* lr。ここに仕込んだ場所から走り始める */
    uint64_t sp;        /* SP_EL1 */
    uint64_t pad;       /* 16 バイト境界を保つ */
} aarch64_context_t;

#define AARCH64_TASK_FREE    0
#define AARCH64_TASK_READY   1
#define AARCH64_TASK_RUNNING 2
#define AARCH64_TASK_DONE    3

typedef struct aarch64_task {
    aarch64_context_t ctx;
    uint64_t ttbr0;         /* ユーザー空間の root PA。0 = カーネルスレッド */
    uint64_t kstack_pa;     /* カーネルスタックの物理先頭 (解放用) */
    uint64_t counter;       /* 何周したか。検証に使う */
    int id;
    int state;
} aarch64_task_t;

#define AARCH64_MAX_TASKS 8

/* 次に走らせるものへ切り替える。**戻ってきたときは自分の番** */
void aarch64_context_switch(aarch64_context_t* next, aarch64_context_t* prev);

void aarch64_task_init(void);
int  aarch64_task_create(void (*entry)(void), uint64_t ttbr0);
void aarch64_task_yield(void);
void aarch64_task_exit(void);
void aarch64_task_on_tick(void);          /* タイマ割り込みから呼ぶ */
void aarch64_task_resched_if_needed(void);/* IRQ の出口で呼ぶ */
aarch64_task_t* aarch64_task_current(void);
uint64_t aarch64_task_switch_count(void);
uint64_t aarch64_task_counter(int id);
int aarch64_task_state(int id);

#endif /* __ASSEMBLER__ */

#endif
