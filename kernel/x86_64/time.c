#include <stdint.h>
#include "x86_64/time.h"
#include "lapic.h"

uint64_t arch_time_now_ms(void) {
    return lapic_get_ticks_ms();
}

uint64_t arch_time_cpu_ms(uint32_t cpu_id) {
    return lapic_get_cpu_ticks_ms(cpu_id);
}
