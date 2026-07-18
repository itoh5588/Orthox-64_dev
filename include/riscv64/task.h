#ifndef ORTHOX_ARCH_RISCV64_TASK_H
#define ORTHOX_ARCH_RISCV64_TASK_H

#include <stdint.h>
#include "task_user_state.h"
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
};

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
    __asm__ volatile("wfi");
}

static inline uint64_t arch_task_read_current_stack_pointer(void) {
    uint64_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    return sp;
}

static inline void arch_task_context_init_fp_state(struct arch_task_context* ctx) {
    (void)ctx;
}

static inline void arch_task_context_copy_fp_state(struct arch_task_context* dst,
                                                   const struct arch_task_context* src) {
    (void)dst;
    (void)src;
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
    arch_task_prepare_fork_child_context(child_ctx, child_frame, parent_frame);
    arch_task_context_copy_fp_state(child_ctx, parent_ctx);
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
