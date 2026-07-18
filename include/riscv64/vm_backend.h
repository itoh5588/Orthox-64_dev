#ifndef ORTHOX_ARCH_RISCV64_VM_BACKEND_H
#define ORTHOX_ARCH_RISCV64_VM_BACKEND_H

#include <stdint.h>
#include "riscv64/trap.h"
#include "riscv64/vm.h"

static inline void arch_vm_backend_init(uint64_t* kernel_root) {
    (void)kernel_root;
}

static inline void arch_vm_backend_map_page(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    riscv64_vm_map_page((uint64_t)(uintptr_t)root, vaddr, paddr, flags);
}

static inline void arch_vm_backend_map_range(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    riscv64_vm_map_range((uint64_t)(uintptr_t)root, vaddr, paddr, size, flags);
}

static inline uint64_t arch_vm_backend_get_phys(uint64_t* root, uint64_t vaddr) {
    return riscv64_vm_get_phys((uint64_t)(uintptr_t)root, vaddr);
}

static inline arch_address_space_t arch_vm_backend_clone_address_space(uint64_t* old_root) {
    (void)old_root;
    return riscv64_vm_clone_kernel_address_space();
}

static inline void arch_vm_backend_destroy_user_address_space(arch_address_space_t address_space) {
    riscv64_vm_destroy_address_space(address_space);
}

static inline void arch_vm_backend_unmap_page(uint64_t* root, uint64_t vaddr) {
    riscv64_vm_unmap_page((uint64_t)(uintptr_t)root, vaddr);
}

static inline void arch_vm_backend_update_page_flags(uint64_t* root, uint64_t vaddr, uint64_t new_flags) {
    riscv64_vm_update_page_flags((uint64_t)(uintptr_t)root, vaddr, new_flags);
}

static inline void arch_vm_backend_handle_page_fault(arch_interrupt_frame_t* frame) {
    (void)frame;
}

#endif
