#ifndef ORTHOX_ARCH_RISCV64_TASK_H
#define ORTHOX_ARCH_RISCV64_TASK_H

#include <stdint.h>
#include "task_user_state.h"
#include "riscv64/boot.h"
#include "riscv64/trap.h"
#include "riscv64/vm.h"

struct syscall_frame;
struct cpu_local;

struct cpu_local* riscv64_task_get_cpu_local_impl(void);
void riscv64_task_set_cpu_local_impl(struct cpu_local* cpu);

struct arch_task_context {
    uint64_t root_pa;
    uint64_t kernel_sp;
    uint64_t sched_ra;
    uint64_t sched_sp;
    uint64_t sched_s0;
    uint64_t sched_s1;
    uint64_t sched_s2;
    uint64_t sched_s3;
    uint64_t sched_s4;
    uint64_t sched_s5;
    uint64_t sched_s6;
    uint64_t sched_s7;
    uint64_t sched_s8;
    uint64_t sched_s9;
    uint64_t sched_s10;
    uint64_t sched_s11;
    riscv64_trap_frame_t user_frame;
    /* **F/D レジスタ (段階 7)。** arch_context_switch (entry.S) が f0-f31 と
     * fcsr を退避・復元する。オフセットは entry.S の RISCV64_CTX_FP と合わせる */
    uint64_t fp[32];
    uint64_t fcsr;
};

#define RISCV64_CTX_FP   416
#define RISCV64_CTX_FCSR (RISCV64_CTX_FP + 32 * 8)
_Static_assert(__builtin_offsetof(struct arch_task_context, fp) == RISCV64_CTX_FP,
               "entry.S の RISCV64_CTX_FP と合わない");
_Static_assert(__builtin_offsetof(struct arch_task_context, fcsr) == RISCV64_CTX_FCSR,
               "entry.S の RISCV64_CTX_FCSR と合わない");

/* 走っている hart の f0-f31/fcsr を ctx へ / ctx から (entry.S) */
void riscv64_fp_save(struct arch_task_context* ctx);
void riscv64_fp_restore(const struct arch_task_context* ctx);

typedef riscv64_trap_frame_t arch_task_exec_frame_t;

static inline uint64_t arch_task_context_get_address_space(const struct arch_task_context* ctx) {
    return ctx ? ctx->root_pa : 0;
}

static inline void arch_task_context_set_address_space(struct arch_task_context* ctx, uint64_t address_space) {
    if (!ctx) return;
    ctx->root_pa = address_space;
}

static inline void arch_task_context_activate_address_space(const struct arch_task_context* ctx) {
    if (!ctx || !ctx->root_pa) return;
    riscv64_vm_activate_address_space(ctx->root_pa);
}

static inline void arch_task_apply_user_tls(uint64_t tls_base) {
    (void)tls_base;
}

static inline struct cpu_local* arch_task_get_cpu_local(void) {
    return riscv64_task_get_cpu_local_impl();
}

static inline void arch_task_set_cpu_local(struct cpu_local* cpu) {
    riscv64_task_set_cpu_local_impl(cpu);
}

static inline void arch_task_prepare_schedule_switch(uint32_t cpu_id,
                                                     uint64_t kernel_stack,
                                                     struct cpu_local* cpu,
                                                     uint64_t tls_base) {
    (void)cpu_id;
    arch_task_set_cpu_local(cpu);
    riscv64_trap_set_kernel_stack(kernel_stack);
    (void)tls_base;
}

static inline void arch_task_activate_user_context(const struct arch_task_context* ctx,
                                                   uint64_t tls_base) {
    arch_task_context_activate_address_space(ctx);
    arch_task_apply_user_tls(tls_base);
}

static inline void arch_task_idle_wait_once(void) {
    // x86 の sti;hlt 相当: 割り込みを許可して wfi し、復帰後に禁止へ戻す
    __asm__ volatile("csrsi sstatus, 0x2");
    riscv64_wfi();
    __asm__ volatile("csrci sstatus, 0x2");
}

static inline uint64_t arch_task_read_current_stack_pointer(void) {
    uint64_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    return sp;
}

/* 新しいタスクと exec 後の像の FP を 0 から始める (aarch64 の P3-3 と同じ)。
 * **前のタスクの f0-f31 を引き継がせない** (誤動作だけでなく漏洩になる)。
 * fcsr = 0 は丸め RNE・例外フラグなしで、musl が期待する初期状態 */
static inline void arch_task_context_init_fp_state(struct arch_task_context* ctx) {
    if (!ctx) return;
    for (int i = 0; i < 32; i++) ctx->fp[i] = 0;
    ctx->fcsr = 0;
}

static inline void arch_task_context_init_kernel_entry(struct arch_task_context* ctx,
                                                       uint64_t entry,
                                                       uint64_t stack_ptr,
                                                       uint64_t address_space) {
    if (!ctx) return;
    ctx->kernel_sp = stack_ptr;
    ctx->sched_ra = entry;
    ctx->sched_sp = stack_ptr;
    ctx->sched_s0 = 0;
    ctx->sched_s1 = 0;
    ctx->sched_s2 = 0;
    ctx->sched_s3 = 0;
    ctx->sched_s4 = 0;
    ctx->sched_s5 = 0;
    ctx->sched_s6 = 0;
    ctx->sched_s7 = 0;
    ctx->sched_s8 = 0;
    ctx->sched_s9 = 0;
    ctx->sched_s10 = 0;
    ctx->sched_s11 = 0;
    arch_task_context_set_address_space(ctx, address_space);
    arch_task_context_init_fp_state(ctx);
}

static inline void arch_task_context_copy_fp_state(struct arch_task_context* dst,
                                                   const struct arch_task_context* src) {
    if (!dst || !src) return;
    for (int i = 0; i < 32; i++) dst->fp[i] = src->fp[i];
    dst->fcsr = src->fcsr;
}

static inline uint64_t arch_task_prepare_kernel_entry(struct arch_task_context* ctx,
                                                      uint64_t kstack_top,
                                                      uint64_t entry,
                                                      uint64_t address_space) {
    if (!ctx) return kstack_top;
    ctx->kernel_sp = kstack_top;
    ctx->sched_ra = entry;
    ctx->sched_sp = kstack_top;
    ctx->sched_s0 = 0;
    ctx->sched_s1 = 0;
    ctx->sched_s2 = 0;
    ctx->sched_s3 = 0;
    ctx->sched_s4 = 0;
    ctx->sched_s5 = 0;
    ctx->sched_s6 = 0;
    ctx->sched_s7 = 0;
    ctx->sched_s8 = 0;
    ctx->sched_s9 = 0;
    ctx->sched_s10 = 0;
    ctx->sched_s11 = 0;
    arch_task_context_set_address_space(ctx, address_space);
    return kstack_top;
}

void riscv64_task_prepare_execve_frame(arch_task_exec_frame_t* frame,
                                       const struct arch_task_user_state* state);
void riscv64_task_prepare_fork_return_frame(arch_task_exec_frame_t* frame,
                                            const arch_task_exec_frame_t* parent_frame);
void riscv64_task_prepare_initial_user_frame(arch_task_exec_frame_t* frame,
                                             const struct arch_task_user_state* state);
void riscv64_task_store_user_frame(struct arch_task_context* ctx,
                                   const arch_task_exec_frame_t* frame);
void riscv64_task_prepare_kernel_resume(struct arch_task_context* ctx,
                                        uint64_t kernel_sp,
                                        uint64_t entry_pc);
void riscv64_task_enter_initial_user_context(const struct arch_task_context* ctx) __attribute__((noreturn));
void riscv64_task_enter_initial_user(const struct arch_task_user_state* state) __attribute__((noreturn));
void riscv64_task_fork_child_return(void) __attribute__((noreturn));

static inline arch_task_exec_frame_t* arch_task_exec_frame_on_kstack(uint64_t kstack_top) {
    return (arch_task_exec_frame_t*)(kstack_top - sizeof(arch_task_exec_frame_t));
}

static inline void arch_task_prepare_execve_frame(arch_task_exec_frame_t* frame,
                                                  const struct arch_task_user_state* state) {
    riscv64_task_prepare_execve_frame(frame, state);
}

static inline void arch_task_commit_execve(struct arch_task_context* ctx,
                                           arch_task_exec_frame_t* frame,
                                           const struct arch_task_user_state* state,
                                           uint64_t tls_base) {
    (void)tls_base;
    arch_task_prepare_execve_frame(frame, state);
    riscv64_task_store_user_frame(ctx, frame);
    // **生のレジスタも 0 にする。** exec は切り替えを通らずに trap 出口から
    // 新しい像へ降りるので、ctx を 0 にしただけでは前の像の f0-f31 が見える
    // (呼び出し元の task_execve が init_fp_state で ctx を 0 にしてある)
    riscv64_fp_restore(ctx);
    // syscall 経由の execve では trap 出口がそのまま sret するため、
    // ここで新しいアドレス空間へ切り替えておく (カーネルはグローバルマップ)
    arch_task_context_activate_address_space(ctx);
}

static inline void arch_task_prepare_fork_child_context(struct arch_task_context* ctx,
                                                        arch_task_exec_frame_t* child_frame,
                                                        const arch_task_exec_frame_t* parent_frame) {
    riscv64_task_prepare_fork_return_frame(child_frame, parent_frame);
    riscv64_task_store_user_frame(ctx, child_frame);
    riscv64_task_prepare_kernel_resume(ctx, ctx->kernel_sp, (uint64_t)riscv64_task_fork_child_return);
}

static inline void arch_task_commit_fork_child(struct arch_task_context* child_ctx,
                                               uint64_t child_kstack_top,
                                               const struct arch_task_context* parent_ctx,
                                               const arch_task_exec_frame_t* parent_frame) {
    arch_task_exec_frame_t* child_frame = arch_task_exec_frame_on_kstack(child_kstack_top);
    if (child_ctx) child_ctx->kernel_sp = child_kstack_top;
    arch_task_prepare_fork_child_context(child_ctx, child_frame, parent_frame);
    // **FP は生のレジスタから取る。** ここは親の fork の syscall の中で、
    // カーネルは FP を使わないので f0-f31 には親のユーザーの値が入っている。
    // parent_ctx->fp は「親が最後に切り替わったときの値」で古い
    (void)parent_ctx;
    riscv64_fp_save(child_ctx);
}

static inline void arch_task_prepare_initial_user_context(struct arch_task_context* ctx,
                                                          const struct arch_task_user_state* state) {
    if (!ctx) return;
    riscv64_task_prepare_initial_user_frame(&ctx->user_frame, state);
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
    riscv64_task_enter_initial_user_context(ctx);
}

#endif
