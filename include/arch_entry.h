#ifndef ORTHOX_ARCH_ENTRY_SELECT_H
#define ORTHOX_ARCH_ENTRY_SELECT_H

#if defined(__riscv)
#include "riscv64/entry.h"
#elif defined(__x86_64__)
#include "x86_64/entry.h"
#else
#error "Unsupported architecture for arch_entry.h"
#endif

#endif
