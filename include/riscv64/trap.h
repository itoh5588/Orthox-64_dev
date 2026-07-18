#ifndef ORTHOX_ARCH_RISCV64_TRAP_H
#define ORTHOX_ARCH_RISCV64_TRAP_H

#include <stdint.h>

typedef struct riscv64_trap_frame {
    uint64_t ra;
    uint64_t sp;
    uint64_t gp;
    uint64_t tp;
    uint64_t t0, t1, t2;
    uint64_t s0, s1;
    uint64_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint64_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint64_t t3, t4, t5, t6;
    uint64_t sepc;
    uint64_t sstatus;
    uint64_t scause;
    uint64_t stval;
    uint64_t reserved0;
} riscv64_trap_frame_t;

typedef riscv64_trap_frame_t arch_interrupt_frame_t;

#define RISCV64_TRAP_FRAME_SIZE 288

#define RISCV64_SCAUSE_INTERRUPT (1ULL << 63)
#define RISCV64_SCAUSE_ECALL_U   8ULL
#define RISCV64_SCAUSE_ECALL_S   9ULL
#define RISCV64_SCAUSE_BREAKPOINT 3ULL
#define RISCV64_SCAUSE_STIMER    (RISCV64_SCAUSE_INTERRUPT | 5ULL)
#define RISCV64_SCAUSE_SSOFT     (RISCV64_SCAUSE_INTERRUPT | 1ULL)
#define RISCV64_SCAUSE_SEXT      (RISCV64_SCAUSE_INTERRUPT | 9ULL)

void riscv64_trap_init(void);
void riscv64_timer_init(void);
void riscv64_trap_dispatch(riscv64_trap_frame_t* frame);
void riscv64_trap_set_kernel_stack(uint64_t kernel_sp);

static inline void riscv64_trap_set_user_return(riscv64_trap_frame_t* frame,
                                                uint64_t pc,
                                                uint64_t sp,
                                                uint64_t arg0,
                                                uint64_t arg1,
                                                uint64_t arg2) {
    if (!frame) return;
    frame->sepc = pc;
    frame->sp = sp;
    frame->a0 = arg0;
    frame->a1 = arg1;
    frame->a2 = arg2;
}

#endif
