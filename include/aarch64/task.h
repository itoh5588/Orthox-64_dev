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
#define AARCH64_CTX_DAIF 104
#define AARCH64_CTX_SIZE 112

/* struct arch_task_context の中で user_frame がどこから始まるか (M3c-2b)。
 * .S から使うので __ASSEMBLER__ の外に置く。**C 側で _Static_assert して
 * いるので、struct を変えたらビルドが落ちる** */
#define AARCH64_TASKCTX_USER_FRAME 128

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct aarch64_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t x29;       /* fp */
    uint64_t x30;       /* lr。ここに仕込んだ場所から走り始める */
    uint64_t sp;        /* SP_EL1 */
    /* **割り込みの開け閉めもタスクの状態 (M4-3 で足した)。**
     * idle は arch_task_idle_wait_once の最後で I を閉じてから譲るので、
     * 復元しないと、起こされた側が割り込みを閉じたまま走り出す。
     * そのままディスクを待つと、完了割り込みが来ず永久に回る
     * (M4-3 のマウントで実際に踏んだ。daif=0x3c0 を実測)。
     * 16 バイト境界のために元から余っていた枠を使っている */
    uint64_t daif;
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

/* 新しいタスクの DAIF の初期値。**いまのマスクから I だけ開けたもの。**
 * 0 (全部開ける) にしないのは、D / A / F の扱いを起動時の判断
 * (start.S の降格や boot.c) から勝手に変えないため */
static inline uint64_t aarch64_daif_for_new_task(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return daif & ~(1ULL << 7);   /* I を開ける */
}

/* 次に走らせるものへ切り替える。**戻ってきたときは自分の番** */
void aarch64_context_switch(aarch64_context_t* next, aarch64_context_t* prev);

void aarch64_task_init(void);
int  aarch64_task_create(void (*entry)(void), uint64_t ttbr0);
void aarch64_task_yield(void);
void aarch64_task_exit(void);
void aarch64_task_on_tick(void);          /* タイマ割り込みから呼ぶ */
void aarch64_task_resched_if_needed(void);/* IRQ の出口で呼ぶ */

/* 切り替えだけを止める区間。**割り込みは開けたまま** (tick は数え続ける)。
 * 出力の 1 行が並行に割れるのを防ぐのに使う。入れ子にできる */
void aarch64_preempt_disable(void);
void aarch64_preempt_enable(void);
int  aarch64_preempt_disabled(void);
aarch64_task_t* aarch64_task_current(void);
uint64_t aarch64_task_switch_count(void);
uint64_t aarch64_task_counter(int id);
int aarch64_task_state(int id);

/* ==========================================================================
 * 共有タスク層 (kernel/task.c / kernel/sched.c) から見た形 (M3c-2a)
 *
 * ここから下は**共有層が要求する arch_* だけ**。上の aarch64_task_* は
 * M3c-1 で自前に作った器で、共有層に載せ替えるまでの足場として残してある。
 * ========================================================================== */

#include "task_user_state.h"
#include "aarch64/trap.h"
#include "aarch64/vm.h"

struct cpu_local;

struct cpu_local* aarch64_task_get_cpu_local_impl(void);
void aarch64_task_set_cpu_local_impl(struct cpu_local* cpu);

/* **regs を先頭に置くこと。** switch.S は AARCH64_CTX_* の offset で
 * 触るので、offset 0 から始まっていないと壊れる。
 *
 * riscv64 は sched_s0-s11 / sched_ra / sched_sp をこの struct に直接
 * 並べているが、aarch64 は M3c-1 で aarch64_context_t を先に作って
 * switch.S と対にしてあるので、それをそのまま埋め込む。 */
struct arch_task_context {
    aarch64_context_t regs;         /* offset 0。switch.S が触るのはここだけ */
    uint64_t root_pa;               /* TTBR0_EL1。0 = カーネルスレッド */
    uint64_t kernel_sp;             /* SP_EL1 の上端 */
    aarch64_trap_frame_t user_frame;
};

_Static_assert(__builtin_offsetof(struct arch_task_context, regs) == 0,
               "switch.S は regs が offset 0 にあることを前提にしている");
_Static_assert(__builtin_offsetof(struct arch_task_context, user_frame) ==
                   AARCH64_TASKCTX_USER_FRAME,
               "entry.S の AARCH64_TASKCTX_USER_FRAME とずれている");

/* 共有スケジューラ (kernel/sched.c) が呼ぶ名前。
 * **regs が offset 0 なので aarch64_context_switch にそのまま渡せる**が、
 * TTBR0 の差し替えが要るので C の薄い層を挟む (riscv64 は .S の中で
 * satp を書いている。あちらは root_pa が offset 0 にあるため) */
void arch_context_switch(struct arch_task_context* next, struct arch_task_context* prev);

/* 共有タスク層へ乗り換える (M3c-2b)。呼ぶと、タイマ割り込みは
 * task_on_timer_tick を叩き、IRQ の出口は schedule() を呼ぶようになる。
 * **M3c-1 の自前の器は同時に止める。** 2 つのスケジューラが同じ CPU を
 * 取り合うと、どちらの前提も成り立たない */
void aarch64_task_use_shared_scheduler(void);
int  aarch64_task_shared_scheduler_on(void);
void aarch64_use_shared_syscalls_on(void);
int  aarch64_use_shared_syscalls(void);

typedef aarch64_trap_frame_t arch_task_exec_frame_t;

static inline uint64_t arch_task_context_get_address_space(const struct arch_task_context* ctx) {
    return ctx ? ctx->root_pa : 0;
}

static inline void arch_task_context_set_address_space(struct arch_task_context* ctx, uint64_t address_space) {
    if (!ctx) return;
    ctx->root_pa = address_space;
}

static inline void arch_task_context_activate_address_space(const struct arch_task_context* ctx) {
    if (!ctx || !ctx->root_pa) return;
    aarch64_vm_activate_address_space(ctx->root_pa);
}

/* TLS。**AArch64 は TPIDR_EL0 が素直に使える** (x86 の fs_base 相当)。
 * まだ使っていないので受け取るだけにしてある */
static inline void arch_task_apply_user_tls(uint64_t tls_base) {
    (void)tls_base;
}

static inline struct cpu_local* arch_task_get_cpu_local(void) {
    return aarch64_task_get_cpu_local_impl();
}

static inline void arch_task_set_cpu_local(struct cpu_local* cpu) {
    aarch64_task_set_cpu_local_impl(cpu);
}

static inline void arch_task_prepare_schedule_switch(uint32_t cpu_id,
                                                     uint64_t kernel_stack,
                                                     struct cpu_local* cpu,
                                                     uint64_t tls_base) {
    (void)cpu_id;
    arch_task_set_cpu_local(cpu);
    aarch64_trap_set_kernel_stack(kernel_stack);
    (void)tls_base;
}

static inline void arch_task_activate_user_context(const struct arch_task_context* ctx,
                                                   uint64_t tls_base) {
    arch_task_context_activate_address_space(ctx);
    arch_task_apply_user_tls(tls_base);
}

/* x86 の sti;hlt / riscv64 の csrsi+wfi 相当。
 * **DAIF の I だけを開けて wfi し、戻ったら閉じる。** 開けずに wfi すると
 * 割り込みが来ても起きない */
static inline void arch_task_idle_wait_once(void) {
    __asm__ volatile("msr daifclr, #2");
    __asm__ volatile("wfi");
    __asm__ volatile("msr daifset, #2");
}

static inline uint64_t arch_task_read_current_stack_pointer(void) {
    uint64_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return sp;
}

/* 浮動小数点。**-mgeneral-regs-only なのでカーネルは FP を使わない。**
 * EL0 で使い始めたら d8-d15 を積む必要がある (日報2026-08-09 追4-1) */
static inline void arch_task_context_init_fp_state(struct arch_task_context* ctx) {
    (void)ctx;
}

static inline void arch_task_context_copy_fp_state(struct arch_task_context* dst,
                                                   const struct arch_task_context* src) {
    (void)dst;
    (void)src;
}

/* **x30 (lr) に入口を仕込むと、最初の切り替えで「そこへ戻る」形で走り出す。**
 * riscv64 が ra に入れるのと同じ理屈 */
static inline void arch_task_context_init_kernel_entry(struct arch_task_context* ctx,
                                                       uint64_t entry,
                                                       uint64_t stack_ptr,
                                                       uint64_t address_space) {
    unsigned i;
    if (!ctx) return;
    for (i = 0; i < sizeof(ctx->regs) / sizeof(uint64_t); i++) {
        ((uint64_t*)&ctx->regs)[i] = 0;
    }
    ctx->kernel_sp = stack_ptr;
    ctx->regs.x30 = entry;
    ctx->regs.sp = stack_ptr;
    /* **割り込みを開けた状態で走り出す。** 0 埋めのままだと D / A / F まで
     * 開いてしまうので、いまのマスクを土台にする */
    ctx->regs.daif = aarch64_daif_for_new_task();
    arch_task_context_set_address_space(ctx, address_space);
    arch_task_context_init_fp_state(ctx);
}

static inline uint64_t arch_task_prepare_kernel_entry(struct arch_task_context* ctx,
                                                      uint64_t kstack_top,
                                                      uint64_t entry,
                                                      uint64_t address_space) {
    if (!ctx) return kstack_top;
    arch_task_context_init_kernel_entry(ctx, entry, kstack_top, address_space);
    return kstack_top;
}

void aarch64_task_prepare_execve_frame(arch_task_exec_frame_t* frame,
                                       const struct arch_task_user_state* state);
void aarch64_task_prepare_fork_return_frame(arch_task_exec_frame_t* frame,
                                            const arch_task_exec_frame_t* parent_frame);
void aarch64_task_prepare_initial_user_frame(arch_task_exec_frame_t* frame,
                                             const struct arch_task_user_state* state);
void aarch64_task_store_user_frame(struct arch_task_context* ctx,
                                   const arch_task_exec_frame_t* frame);
void aarch64_task_prepare_kernel_resume(struct arch_task_context* ctx,
                                        uint64_t kernel_sp,
                                        uint64_t entry_pc);
void aarch64_task_enter_initial_user_context(const struct arch_task_context* ctx) __attribute__((noreturn));
void aarch64_task_fork_child_return(void) __attribute__((noreturn));

static inline arch_task_exec_frame_t* arch_task_exec_frame_on_kstack(uint64_t kstack_top) {
    return (arch_task_exec_frame_t*)(uintptr_t)(kstack_top - sizeof(arch_task_exec_frame_t));
}

static inline void arch_task_prepare_execve_frame(arch_task_exec_frame_t* frame,
                                                  const struct arch_task_user_state* state) {
    aarch64_task_prepare_execve_frame(frame, state);
}

static inline void arch_task_commit_execve(struct arch_task_context* ctx,
                                           arch_task_exec_frame_t* frame,
                                           const struct arch_task_user_state* state,
                                           uint64_t tls_base) {
    (void)tls_base;
    arch_task_prepare_execve_frame(frame, state);
    aarch64_task_store_user_frame(ctx, frame);
    /* svc 経由の execve は例外の出口がそのまま eret するので、ここで
     * 新しいアドレス空間へ切り替えておく。**カーネルは TTBR1 なので
     * TTBR0 を差し替えても自分の足元は動かない** */
    arch_task_context_activate_address_space(ctx);
}

static inline void arch_task_prepare_fork_child_context(struct arch_task_context* ctx,
                                                        arch_task_exec_frame_t* child_frame,
                                                        const arch_task_exec_frame_t* parent_frame) {
    aarch64_task_prepare_fork_return_frame(child_frame, parent_frame);
    aarch64_task_store_user_frame(ctx, child_frame);
    aarch64_task_prepare_kernel_resume(ctx, ctx->kernel_sp,
                                       (uint64_t)(uintptr_t)aarch64_task_fork_child_return);
}

static inline void arch_task_commit_fork_child(struct arch_task_context* child_ctx,
                                               uint64_t child_kstack_top,
                                               const struct arch_task_context* parent_ctx,
                                               const arch_task_exec_frame_t* parent_frame) {
    arch_task_exec_frame_t* child_frame = arch_task_exec_frame_on_kstack(child_kstack_top);
    if (child_ctx) child_ctx->kernel_sp = child_kstack_top;
    arch_task_prepare_fork_child_context(child_ctx, child_frame, parent_frame);
    arch_task_context_copy_fp_state(child_ctx, parent_ctx);
}

static inline void arch_task_prepare_initial_user_context(struct arch_task_context* ctx,
                                                          const struct arch_task_user_state* state) {
    if (!ctx) return;
    aarch64_task_prepare_initial_user_frame(&ctx->user_frame, state);
}

static inline void arch_task_sync_user_state(struct arch_task_context* ctx,
                                             const struct arch_task_user_state* state) {
    arch_task_prepare_initial_user_context(ctx, state);
}

static inline int arch_task_enter_initial_user(const struct arch_task_user_state* state,
                                               const struct arch_task_context* ctx,
                                               uint64_t* os_stack_ptr) {
    (void)state;
    (void)os_stack_ptr;
    aarch64_task_enter_initial_user_context(ctx);
}

#endif /* __ASSEMBLER__ */

#endif
