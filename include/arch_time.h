#ifndef ORTHOX_ARCH_TIME_SELECT_H
#define ORTHOX_ARCH_TIME_SELECT_H

#if defined(__riscv)
#include "riscv64/time.h"
#elif defined(__x86_64__)
#include "x86_64/time.h"
#else
#error "Unsupported architecture for arch_time.h"
#endif

#endif
