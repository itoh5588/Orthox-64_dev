#ifndef ORTHOX_ARCH_MMU_SELECT_H
#define ORTHOX_ARCH_MMU_SELECT_H

#if defined(__riscv)
#include "riscv64/mmu.h"
#elif defined(__x86_64__)
#include "x86_64/mmu.h"
#else
#error "Unsupported architecture for arch_mmu.h"
#endif

#endif
