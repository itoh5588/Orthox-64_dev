#ifndef ORTHOX_ARCH_TRAP_H
#define ORTHOX_ARCH_TRAP_H

#include <stdint.h>

typedef struct arch_interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t int_no, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} arch_interrupt_frame_t;

void arch_trap_init_bsp(void);
void arch_trap_init_ap(uint32_t cpu_id);

#endif
