#ifndef ORTHOX_ARCH_SYSCALL_SELECT_H
#define ORTHOX_ARCH_SYSCALL_SELECT_H

#if defined(__riscv)
#include "riscv64/syscall.h"
#elif defined(__aarch64__)
#include "aarch64/syscall.h"
#elif defined(__x86_64__)
#include "x86_64/syscall.h"
#else
#error "Unsupported architecture for arch_syscall.h"
#endif

#endif
