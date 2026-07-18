#ifndef ORTHOX_ARCH_VM_BACKEND_SELECT_H
#define ORTHOX_ARCH_VM_BACKEND_SELECT_H

#if defined(__riscv)
#include "riscv64/vm_backend.h"
#elif defined(__x86_64__)
#include "x86_64/vm_backend.h"
#else
#error "Unsupported architecture for arch_vm_backend.h"
#endif

#endif
