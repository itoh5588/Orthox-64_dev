#ifndef ORTHOX_ARCH_ENTRY_H
#define ORTHOX_ARCH_ENTRY_H

#include <stdint.h>
#include "x86_64/task.h"
#include "x86_64/trap.h"

void arch_context_switch(struct arch_task_context* next_ctx, struct arch_task_context* prev_ctx);
int arch_enter_user(int argc, char** argv, uint16_t ss, uint64_t rip, uint64_t rsp, uint64_t* os_stack_ptr);
void arch_fork_child_return(void);
void arch_syscall_entry(void);
void arch_interrupt_dispatch(arch_interrupt_frame_t* frame);

static inline void arch_syscall_init_cpu(void) {
    uint32_t low;
    uint32_t high;
    uint64_t efer;
    uint64_t star;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080U));
    efer = ((uint64_t)high << 32) | low;
    efer |= 1ULL;
    low = (uint32_t)(efer & 0xFFFFFFFFU);
    high = (uint32_t)(efer >> 32);
    __asm__ volatile("wrmsr" : : "c"(0xC0000080U), "a"(low), "d"(high));

    star = (0x10ULL << 48) | (0x08ULL << 32);
    low = (uint32_t)(star & 0xFFFFFFFFU);
    high = (uint32_t)(star >> 32);
    __asm__ volatile("wrmsr" : : "c"(0xC0000081U), "a"(low), "d"(high));

    {
        uint64_t lstar = (uint64_t)(uintptr_t)arch_syscall_entry;
        low = (uint32_t)(lstar & 0xFFFFFFFFU);
        high = (uint32_t)(lstar >> 32);
        __asm__ volatile("wrmsr" : : "c"(0xC0000082U), "a"(low), "d"(high));
    }

    __asm__ volatile("wrmsr" : : "c"(0xC0000084U), "a"(0x200U), "d"(0U));
}

#endif
