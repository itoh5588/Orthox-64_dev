#ifndef ORTHOX_ARCH_TIME_H
#define ORTHOX_ARCH_TIME_H

#include <stdint.h>

uint64_t arch_time_now_ms(void);
uint64_t arch_time_cpu_ms(uint32_t cpu_id);

#endif
