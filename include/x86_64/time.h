#ifndef ORTHOX_ARCH_TIME_H
#define ORTHOX_ARCH_TIME_H

#include <stdint.h>
#include "lapic.h"

/* arch_time_* for x86_64: thin inline wrappers over the existing lapic time
 * source. Defined inline so the kernel/x86_64/time.c translation unit is not
 * required to be linked for the x86 build path. */

static inline uint64_t arch_time_now_ms(void) {
    return lapic_get_ticks_ms();
}

static inline uint64_t arch_time_cpu_ms(uint32_t cpu_id) {
    return lapic_get_cpu_ticks_ms(cpu_id);
}

#endif
