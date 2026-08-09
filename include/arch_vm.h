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

#elif defined(__aarch64__)
#include "aarch64/vm.h"

/* AArch64 は「読み書き可否」と「実行可否」が別のビットで、しかも
 * **EL0 の実行禁止 (UXN) と EL1 の実行禁止 (PXN) が別**。
 * 組み立ては aarch64_vm_user_page_attr に集めてある (PXN は常に立てる) */
static inline uint64_t arch_vm_user_page_flags(int writable, int executable) {
    return aarch64_vm_user_page_attr(writable, executable);
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
