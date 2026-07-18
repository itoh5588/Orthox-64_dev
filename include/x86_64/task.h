#ifndef ORTHOX_ARCH_TASK_H
#define ORTHOX_ARCH_TASK_H

#include <stdint.h>
#include "task_user_state.h"
#include "gdt.h"
#include "x86_64/mmu.h"
#include "x86_64/syscall.h"

#define ARCH_TASK_KERNEL_CS 0x08U
#define ARCH_TASK_KERNEL_SS 0x10U
#define ARCH_TASK_USER_SS   0x1BU
#define ARCH_TASK_MSR_FS_BASE 0xC0000100U

struct arch_task_context {
    uint64_t cr3, rip, rflags, reserved1; // 0, 8, 16, 24
    uint64_t cs, ss, fs, gs;              // 32, 40, 48, 56
    uint64_t rax, rbx, rcx, rdx, rdi, rsi, rsp, rbp; // 64..120
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;   // 128..184
    uint8_t fxsave_area[512] __attribute__((aligned(16))); // 192
} __attribute__((packed));

typedef arch_syscall_frame_t arch_task_exec_frame_t;

struct cpu_local;

int arch_enter_user(int argc, char** argv, uint16_t ss, uint64_t rip, uint64_t rsp,
                    uint64_t* os_stack_ptr);
void arch_fork_child_return(void);

static inline uint64_t arch_task_context_get_address_space(const struct arch_task_context* ctx) {
    return ctx ? ctx->cr3 : 0;
}

static inline void arch_task_context_set_address_space(struct arch_task_context* ctx, uint64_t address_space) {
    if (!ctx) return;
    ctx->cr3 = address_space;
}

static inline void arch_task_context_activate_address_space(const struct arch_task_context* ctx) {
    uint64_t address_space = arch_task_context_get_address_space(ctx);
    arch_mmu_write_address_space(address_space);
}

static inline void arch_task_apply_user_tls(uint64_t tls_base) {
    uint32_t low = (uint32_t)(tls_base & 0xFFFFFFFFU);
    uint32_t high = (uint32_t)(tls_base >> 32);
    __asm__ volatile("wrmsr" : : "c"(ARCH_TASK_MSR_FS_BASE), "a"(low), "d"(high));
}

static inline struct cpu_local* arch_task_get_cpu_local(void) {
    uint32_t low;
    uint32_t high;
    uint64_t value;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000101U));
    value = ((uint64_t)high << 32) | low;
    if (value) return (struct cpu_local*)(uintptr_t)value;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000102U));
    value = ((uint64_t)high << 32) | low;
    return (struct cpu_local*)(uintptr_t)value;
}

static inline void arch_task_set_cpu_local(struct cpu_local* cpu) {
    uint64_t value = (uint64_t)(uintptr_t)cpu;
    uint32_t low = (uint32_t)(value & 0xFFFFFFFFU);
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(0xC0000101U), "a"(low), "d"(high));
    __asm__ volatile("wrmsr" : : "c"(0xC0000102U), "a"(low), "d"(high));
}

static inline void arch_task_prepare_schedule_switch(uint32_t cpu_id,
                                                     uint64_t kernel_stack,
                                                     struct cpu_local* cpu,
                                                     uint64_t tls_base) {
    tss_set_stack_for_cpu(cpu_id, kernel_stack);
    arch_task_set_cpu_local(cpu);
    arch_task_apply_user_tls(tls_base);
}

static inline void arch_task_activate_user_context(const struct arch_task_context* ctx,
                                                   uint64_t tls_base) {
    arch_task_context_activate_address_space(ctx);
    arch_task_apply_user_tls(tls_base);
}

static inline void arch_task_idle_wait_once(void) {
    __asm__ volatile("sti");
    __asm__ volatile("hlt");
}

static inline uint64_t arch_task_read_current_stack_pointer(void) {
    uint64_t sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    return sp;
}

static inline void arch_task_context_init_fp_state(struct arch_task_context* ctx) {
    if (!ctx) return;
    ctx->fxsave_area[0] = 0x7f;
    ctx->fxsave_area[1] = 0x03;
    ctx->fxsave_area[24] = 0x80;
    ctx->fxsave_area[25] = 0x1f;
}

static inline void arch_task_context_init_kernel_entry(struct arch_task_context* ctx,
                                                       uint64_t entry,
                                                       uint64_t stack_ptr,
                                                       uint64_t address_space) {
    if (!ctx) return;
    ctx->rip = entry;
    ctx->rsp = stack_ptr;
    ctx->cs = ARCH_TASK_KERNEL_CS;
    ctx->ss = ARCH_TASK_KERNEL_SS;
    ctx->rflags = 0;
    arch_task_context_set_address_space(ctx, address_space);
    arch_task_context_init_fp_state(ctx);
}

static inline uint64_t arch_task_prepare_kernel_entry(struct arch_task_context* ctx,
                                                      uint64_t kstack_top,
                                                      uint64_t entry,
                                                      uint64_t address_space) {
    uint64_t* sp = (uint64_t*)(kstack_top - 8);
    *sp = entry;
    *(--sp) = 0x202;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    arch_task_context_init_kernel_entry(ctx, entry, (uint64_t)sp, address_space);
    return (uint64_t)sp;
}

static inline void arch_task_context_copy_fp_state(struct arch_task_context* dst,
                                                   const struct arch_task_context* src) {
    if (!dst || !src) return;
    for (int i = 0; i < 512; i++) dst->fxsave_area[i] = src->fxsave_area[i];
}

static inline arch_task_exec_frame_t* arch_task_exec_frame_on_kstack(uint64_t kstack_top) {
    return (arch_task_exec_frame_t*)(kstack_top - sizeof(arch_task_exec_frame_t));
}

static inline void arch_task_prepare_execve_frame(arch_task_exec_frame_t* frame,
                                                  const struct arch_task_user_state* state) {
    if (!frame) return;
    frame->r15 = 0;
    frame->r14 = 0;
    frame->r13 = 0;
    frame->r12 = 0;
    frame->rbp = 0;
    frame->rbx = 0;
    arch_syscall_set_arg5(frame, 0);
    arch_syscall_set_arg4(frame, 0);
    arch_syscall_set_arg3(frame, 0);
    arch_syscall_set_return(frame, 0);
    arch_syscall_set_program_counter(frame, state ? state->entry_pc : 0);
    arch_syscall_set_stack_pointer(frame, state ? state->user_sp : 0);
    arch_syscall_set_arg0(frame, state ? state->arg0 : 0);
    arch_syscall_set_arg1(frame, state ? state->arg1 : 0);
    arch_syscall_set_arg2(frame, state ? state->arg2 : 0);
}

static inline void arch_task_commit_execve(struct arch_task_context* ctx,
                                           arch_task_exec_frame_t* frame,
                                           const struct arch_task_user_state* state,
                                           uint64_t tls_base) {
    arch_task_prepare_execve_frame(frame, state);
    arch_task_activate_user_context(ctx, tls_base);
}

static inline void arch_task_prepare_fork_child_context(struct arch_task_context* ctx,
                                                        arch_task_exec_frame_t* child_frame,
                                                        const arch_task_exec_frame_t* parent_frame) {
    uint64_t* sp;
    if (!ctx || !child_frame) return;
    if (parent_frame) {
        const uint8_t* src = (const uint8_t*)parent_frame;
        uint8_t* dst = (uint8_t*)child_frame;
        for (uint64_t i = 0; i < sizeof(*child_frame); i++) dst[i] = src[i];
    }
    arch_syscall_set_return(child_frame, 0);
    sp = (uint64_t*)child_frame;
    *(--sp) = (uint64_t)arch_fork_child_return;
    *(--sp) = 0x202;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    ctx->rip = (uint64_t)arch_fork_child_return;
    ctx->rsp = (uint64_t)sp;
    ctx->rflags = 0x202;
    ctx->cs = ARCH_TASK_KERNEL_CS;
    ctx->ss = ARCH_TASK_KERNEL_SS;
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
    (void)ctx;
    (void)state;
}

static inline void arch_task_sync_user_state(struct arch_task_context* ctx,
                                             const struct arch_task_user_state* state) {
    (void)ctx;
    (void)state;
}

static inline int arch_task_enter_initial_user(const struct arch_task_user_state* state,
                                               const struct arch_task_context* ctx,
                                               uint64_t* os_stack_ptr) {
    (void)ctx;
    return arch_enter_user(state ? (int)state->arg0 : 0,
                           state ? (char**)state->arg1 : 0,
                           ARCH_TASK_USER_SS,
                           state ? state->entry_pc : 0,
                           state ? state->user_sp : 0,
                           os_stack_ptr);
}

#endif
