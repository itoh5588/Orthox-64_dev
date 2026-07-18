#ifndef ORTHOX_ARCH_X86_64_SYSCALL_H
#define ORTHOX_ARCH_X86_64_SYSCALL_H

#include <stdint.h>

struct arch_task_context;

struct arch_syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef struct arch_syscall_frame arch_syscall_frame_t;

static inline uint64_t arch_syscall_number(const arch_syscall_frame_t* frame) {
    return frame ? frame->rax : 0;
}

static inline uint64_t arch_syscall_arg0(const arch_syscall_frame_t* frame) {
    return frame ? frame->rdi : 0;
}

static inline uint64_t arch_syscall_arg1(const arch_syscall_frame_t* frame) {
    return frame ? frame->rsi : 0;
}

static inline uint64_t arch_syscall_arg2(const arch_syscall_frame_t* frame) {
    return frame ? frame->rdx : 0;
}

static inline uint64_t arch_syscall_arg3(const arch_syscall_frame_t* frame) {
    return frame ? frame->r10 : 0;
}

static inline uint64_t arch_syscall_arg4(const arch_syscall_frame_t* frame) {
    return frame ? frame->r8 : 0;
}

static inline uint64_t arch_syscall_arg5(const arch_syscall_frame_t* frame) {
    return frame ? frame->r9 : 0;
}

static inline void arch_syscall_set_number(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->rax = value;
}

static inline void arch_syscall_set_arg0(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->rdi = value;
}

static inline void arch_syscall_set_arg1(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->rsi = value;
}

static inline void arch_syscall_set_arg2(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->rdx = value;
}

static inline void arch_syscall_set_arg3(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->r10 = value;
}

static inline void arch_syscall_set_arg4(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->r8 = value;
}

static inline void arch_syscall_set_arg5(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->r9 = value;
}

static inline void arch_syscall_set_return(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->rax = value;
}

static inline uint64_t arch_syscall_return(const arch_syscall_frame_t* frame) {
    return frame ? frame->rax : 0;
}

static inline uint64_t arch_syscall_program_counter(const arch_syscall_frame_t* frame) {
    return frame ? frame->rip : 0;
}

static inline void arch_syscall_set_program_counter(arch_syscall_frame_t* frame, uint64_t pc) {
    if (!frame) return;
    frame->rip = pc;
}

static inline uint64_t arch_syscall_stack_pointer(const arch_syscall_frame_t* frame) {
    return frame ? frame->rsp : 0;
}

static inline void arch_syscall_set_stack_pointer(arch_syscall_frame_t* frame, uint64_t sp) {
    if (!frame) return;
    frame->rsp = sp;
}

#endif
