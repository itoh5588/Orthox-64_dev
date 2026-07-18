#ifndef ORTHOX_ARCH_RISCV64_PLATFORM_H
#define ORTHOX_ARCH_RISCV64_PLATFORM_H

#include <stdint.h>

static inline void arch_platform_init_bsp(void) {
}

static inline void arch_platform_init_ap(uint32_t cpu_id, uint64_t kernel_stack) {
    (void)cpu_id;
    (void)kernel_stack;
}

static inline void arch_platform_send_resched_ipi(uint32_t arch_cpu_id) {
    (void)arch_cpu_id;
}

#endif
