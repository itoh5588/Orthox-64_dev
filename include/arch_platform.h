#ifndef ORTHOX_ARCH_PLATFORM_SELECT_H
#define ORTHOX_ARCH_PLATFORM_SELECT_H

#if defined(__riscv)
#include "riscv64/platform.h"
#elif defined(__x86_64__)
#include "x86_64/platform.h"
#else
#error "Unsupported architecture for arch_platform.h"
#endif

#endif
