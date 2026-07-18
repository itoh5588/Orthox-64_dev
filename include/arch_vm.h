#ifndef ORTHOX_ARCH_VM_SELECT_H
#define ORTHOX_ARCH_VM_SELECT_H

#include <stdint.h>

#if defined(__riscv)
#include "riscv64/vm.h"

static inline uint64_t arch_vm_user_page_flags(int writable, int executable) {
    uint64_t flags = RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_U;
    if (writable) flags |= RISCV64_VM_PAGE_W;
    if (executable) flags |= RISCV64_VM_PAGE_X;
    return flags;
}

#elif defined(__x86_64__)
#include "x86_64/vm.h"
#include "vmm.h"

static inline uint64_t arch_vm_user_page_flags(int writable, int executable) {
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (writable) flags |= PTE_WRITABLE;
    (void)executable;
    return flags;
}

#else
#error "Unsupported architecture for arch_vm.h"
#endif

#endif
