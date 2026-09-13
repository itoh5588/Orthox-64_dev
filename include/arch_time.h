#ifndef ORTHOX_ARCH_TIME_SELECT_H
#define ORTHOX_ARCH_TIME_SELECT_H

#if defined(__riscv)
#include "riscv64/time.h"
#elif defined(__aarch64__)
#include "aarch64/time.h"
#elif defined(__x86_64__)
#include "x86_64/time.h"
#else
#error "Unsupported architecture for arch_time.h"
#endif

/* **電池で動く時計 (RTC) を持つ機械だけが答える壁時計 (2026-09-13)。**
 * Unix 秒、持たなければ 0。x86 は CMOS (kernel/x86_64/sys_time.c)、aarch64 /
 * riscv64 は持たないので共有層の弱い既定 (kernel/sys_task.c) が 0 を返す。
 * clock_gettime(CLOCK_REALTIME) が SNTP の次に見る */
uint64_t arch_rtc_seconds(void);

#endif
