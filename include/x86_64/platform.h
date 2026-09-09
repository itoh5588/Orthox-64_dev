#ifndef ORTHOX_ARCH_PLATFORM_H
#define ORTHOX_ARCH_PLATFORM_H

#include <stdint.h>

void arch_platform_init_bsp(void);
void arch_platform_init_ap(uint32_t cpu_id, uint64_t kernel_stack);
void arch_platform_send_resched_ipi(uint32_t arch_cpu_id);

#endif
