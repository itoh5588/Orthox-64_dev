#ifndef ORTHOX_ARCH_RISCV64_ENTRY_H
#define ORTHOX_ARCH_RISCV64_ENTRY_H

#include <stdint.h>
#include "riscv64/trap.h"
#include "riscv64/task.h"

void riscv64_enter_user_from_frame(riscv64_trap_frame_t* frame) __attribute__((noreturn));
void riscv64_run_on_stack(uint64_t stack_top, void (*entry)(void)) __attribute__((noreturn));
void riscv64_activate_address_space_and_enter_user(uint64_t root_pa,
                                                   riscv64_trap_frame_t* frame) __attribute__((noreturn));
void riscv64_prepare_initial_user_frame(riscv64_trap_frame_t* frame,
                                        uint64_t entry_pc,
                                        uint64_t user_sp,
                                        uint64_t arg0,
                                        uint64_t arg1,
                                        uint64_t arg2);
void riscv64_prepare_fork_return_frame(riscv64_trap_frame_t* frame,
                                       uint64_t entry_pc,
                                       uint64_t user_sp);
void arch_context_switch(struct arch_task_context* next_ctx, struct arch_task_context* prev_ctx);

static inline void arch_syscall_init_cpu(void) {
}

#endif
