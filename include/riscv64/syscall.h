#ifndef ORTHOX_ARCH_RISCV64_SYSCALL_H
#define ORTHOX_ARCH_RISCV64_SYSCALL_H

#include <stdint.h>
#include "riscv64/trap.h"

struct arch_task_context;

typedef riscv64_trap_frame_t arch_syscall_frame_t;

void riscv64_syscall_dispatch(riscv64_trap_frame_t* frame);
void riscv64_syscall_sync_current_user_frame(const riscv64_trap_frame_t* frame);
void riscv64_syscall_set_current_context(struct arch_task_context* ctx);

static inline uint64_t arch_syscall_number(const arch_syscall_frame_t* frame) {
    return frame ? frame->a7 : 0;
}

static inline uint64_t arch_syscall_arg0(const arch_syscall_frame_t* frame) {
    return frame ? frame->a0 : 0;
}

static inline uint64_t arch_syscall_arg1(const arch_syscall_frame_t* frame) {
    return frame ? frame->a1 : 0;
}

static inline uint64_t arch_syscall_arg2(const arch_syscall_frame_t* frame) {
    return frame ? frame->a2 : 0;
}

static inline uint64_t arch_syscall_arg3(const arch_syscall_frame_t* frame) {
    return frame ? frame->a3 : 0;
}

static inline uint64_t arch_syscall_arg4(const arch_syscall_frame_t* frame) {
    return frame ? frame->a4 : 0;
}

static inline uint64_t arch_syscall_arg5(const arch_syscall_frame_t* frame) {
    return frame ? frame->a5 : 0;
}

static inline void arch_syscall_set_number(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->a7 = value;
}

static inline void arch_syscall_set_arg0(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->a0 = value;
}

static inline void arch_syscall_set_arg1(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->a1 = value;
}

static inline void arch_syscall_set_arg2(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->a2 = value;
}

static inline void arch_syscall_set_arg3(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->a3 = value;
}

static inline void arch_syscall_set_arg4(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->a4 = value;
}

static inline void arch_syscall_set_arg5(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->a5 = value;
}

static inline void arch_syscall_set_return(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->a0 = value;
}

static inline uint64_t arch_syscall_return(const arch_syscall_frame_t* frame) {
    return frame ? frame->a0 : 0;
}

static inline uint64_t arch_syscall_program_counter(const arch_syscall_frame_t* frame) {
    return frame ? frame->sepc : 0;
}

static inline void arch_syscall_set_program_counter(arch_syscall_frame_t* frame, uint64_t pc) {
    if (!frame) return;
    frame->sepc = pc;
}

static inline uint64_t arch_syscall_stack_pointer(const arch_syscall_frame_t* frame) {
    return frame ? frame->sp : 0;
}

static inline void arch_syscall_set_stack_pointer(arch_syscall_frame_t* frame, uint64_t sp) {
    if (!frame) return;
    frame->sp = sp;
}

#endif
