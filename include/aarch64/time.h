#ifndef ORTHOX_ARCH_AARCH64_TIME_H
#define ORTHOX_ARCH_AARCH64_TIME_H

#include <stdint.h>

/* 共有スケジューラ (kernel/sched.c) と待ち層が使う時刻 (M3c-2a)。
 * generic timer の tick から出す */
uint64_t arch_time_now_ms(void);
uint64_t arch_time_cpu_ms(uint32_t cpu_id);

#endif
