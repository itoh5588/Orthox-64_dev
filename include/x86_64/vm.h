#ifndef ORTHOX_ARCH_VM_H
#define ORTHOX_ARCH_VM_H

#include <stdint.h>
#include "vmm.h"
#include "pmm.h"

typedef uint64_t arch_address_space_t;

/* arch_vm_* API for x86_64: thin wrappers over the existing vmm_* layer.
 * address_space is a physical PML4 address (CR3 value), matching main's ctx.cr3.
 * Keeping the vmm.c implementation intact preserves main's evolved VM features
 * (CoW, memtrace, COW fault handling) while letting task/syscall code use the
 * arch-agnostic arch_vm_* surface. */

static inline arch_address_space_t arch_vm_kernel_address_space(void) {
    return (arch_address_space_t)vmm_get_kernel_pml4_phys();
}

static inline uint64_t* arch_vm_address_space_root(arch_address_space_t address_space) {
    return (uint64_t*)PHYS_TO_VIRT((void*)(uintptr_t)address_space);
}

static inline arch_address_space_t arch_vm_create_user_address_space(void) {
    /* Allocate a fresh PML4 and copy the kernel half (upper 256 entries) from
     * the kernel PML4 so kernel mappings are inherited by the new user space.
     * The lower 256 entries (user space) are zeroed. */
    void* pml4_phys = pmm_alloc(1);
    if (!pml4_phys) return 0;
    uint64_t* new_root = (uint64_t*)PHYS_TO_VIRT(pml4_phys);
    uint64_t* kernel_root = arch_vm_address_space_root(arch_vm_kernel_address_space());
    for (int i = 0; i < 512; i++) {
        new_root[i] = (i >= 256) ? kernel_root[i] : 0;
    }
    return (arch_address_space_t)(uint64_t)pml4_phys;
}

static inline arch_address_space_t arch_vm_clone_address_space(arch_address_space_t address_space) {
    return (arch_address_space_t)vmm_copy_pml4(arch_vm_address_space_root(address_space));
}

static inline void arch_vm_destroy_user_address_space(arch_address_space_t address_space) {
    vmm_free_user_pml4((uint64_t)address_space);
}

static inline void arch_vm_map_page(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    vmm_map_page(arch_vm_address_space_root(address_space), vaddr, paddr, flags);
}

static inline void arch_vm_map_range(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    vmm_map_range(arch_vm_address_space_root(address_space), vaddr, paddr, size, flags);
}

static inline uint64_t arch_vm_get_phys(arch_address_space_t address_space, uint64_t vaddr) {
    return vmm_get_phys(arch_vm_address_space_root(address_space), vaddr);
}

static inline void arch_vm_unmap_page(arch_address_space_t address_space, uint64_t vaddr) {
    /* vmm.c has no unmap helper; upper layers handle unmap via remap. Stub. */
    (void)address_space;
    (void)vaddr;
}

static inline void arch_vm_update_page_flags(arch_address_space_t address_space, uint64_t vaddr, uint64_t new_flags) {
    /* vmm.c handles flag updates through remap paths; no direct API. Stub. */
    (void)address_space;
    (void)vaddr;
    (void)new_flags;
}

#endif
