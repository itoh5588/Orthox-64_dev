#ifndef ORTHOX_ARCH_TRAP_SELECT_H
#define ORTHOX_ARCH_TRAP_SELECT_H

#if defined(__riscv)
#include "riscv64/trap.h"
#elif defined(__x86_64__)
#include "x86_64/trap.h"
#else
#error "Unsupported architecture for arch_trap.h"
#endif

#endif
