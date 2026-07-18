#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/entry.h"
#include "riscv64/task.h"
#include "syscall.h"
#include "task.h"

static struct cpu_local* g_riscv64_cpu_local;

struct cpu_local* riscv64_task_get_cpu_local_impl(void) {
    return g_riscv64_cpu_local;
}

void riscv64_task_set_cpu_local_impl(struct cpu_local* cpu) {
    g_riscv64_cpu_local = cpu;
}

static void riscv64_task_copy_frame(arch_task_exec_frame_t* dst,
                                    const arch_task_exec_frame_t* src) {
    const uint8_t* in = (const uint8_t*)src;
    uint8_t* out = (uint8_t*)dst;
    if (!dst || !src) return;
    for (uint64_t i = 0; i < sizeof(*dst); i++) {
        out[i] = in[i];
    }
}

static void riscv64_task_fill_user_frame(arch_task_exec_frame_t* frame,
                                         const struct arch_task_user_state* state) {
    if (!frame || !state) return;
    riscv64_prepare_initial_user_frame(frame,
                                       state->entry_pc,
                                       state->user_sp,
                                       state->arg0,
                                       state->arg1,
                                       state->arg2);
}

void riscv64_task_prepare_execve_frame(arch_task_exec_frame_t* frame,
                                       const struct arch_task_user_state* state) {
    riscv64_task_fill_user_frame(frame, state);
}

void riscv64_task_prepare_fork_return_frame(arch_task_exec_frame_t* frame,
                                            const arch_task_exec_frame_t* parent_frame) {
    if (!frame) return;
    if (parent_frame) {
        riscv64_task_copy_frame(frame, parent_frame);
    } else {
        riscv64_prepare_fork_return_frame(frame, 0, 0);
    }
    frame->a0 = 0;
}

void riscv64_task_prepare_initial_user_frame(arch_task_exec_frame_t* frame,
                                             const struct arch_task_user_state* state) {
    riscv64_task_fill_user_frame(frame, state);
}

void riscv64_task_store_user_frame(struct arch_task_context* ctx,
                                   const arch_task_exec_frame_t* frame) {
    if (!ctx || !frame) return;
    riscv64_task_copy_frame(&ctx->user_frame, frame);
}

void riscv64_task_prepare_kernel_resume(struct arch_task_context* ctx,
                                        uint64_t kernel_sp,
                                        uint64_t entry_pc) {
    if (!ctx) return;
    ctx->kernel_sp = kernel_sp;
    ctx->sched_ra = entry_pc;
    ctx->sched_sp = kernel_sp;
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
}

void riscv64_task_fork_child_return(void) {
    struct task* current = get_current_task();
    if (!current) {
        riscv64_wait_forever();
    }
    riscv64_task_enter_initial_user_context(&current->ctx);
}

__attribute__((noinline, disable_tail_calls))
void riscv64_task_enter_initial_user_context(const struct arch_task_context* ctx) {
    volatile riscv64_trap_frame_t initial_frame;
    uint64_t current_root = riscv64_vm_current_address_space();
    uint64_t kernel_root = riscv64_vm_kernel_address_space();
    uint64_t root_pa;
    uint64_t kernel_sp;
    if (!ctx) {
        for (;;) {
            __asm__ volatile("wfi");
        }
    }
    root_pa = ctx->root_pa;
    kernel_sp = ctx->kernel_sp;
    riscv64_task_copy_frame((arch_task_exec_frame_t*)&initial_frame, &ctx->user_frame);
    riscv64_uart_puts("  enter user ctx root: 0x");
    riscv64_uart_puthex64(root_pa);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  enter user kernel : 0x");
    riscv64_uart_puthex64(kernel_root);
    riscv64_uart_puts("\n");
    riscv64_trap_set_kernel_stack(kernel_sp);
    if (root_pa != current_root) {
        riscv64_activate_address_space_and_enter_user(root_pa, (riscv64_trap_frame_t*)&initial_frame);
    }
    riscv64_enter_user_from_frame((riscv64_trap_frame_t*)&initial_frame);
}

void riscv64_task_enter_initial_user(const struct arch_task_user_state* state) {
    riscv64_trap_frame_t frame;

    riscv64_task_fill_user_frame(&frame, state);
    riscv64_enter_user_from_frame(&frame);
}
