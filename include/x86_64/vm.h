#ifndef ORTHOX_ARCH_VM_H
#define ORTHOX_ARCH_VM_H

#include <stdint.h>

typedef uint64_t arch_address_space_t;

arch_address_space_t arch_vm_kernel_address_space(void);
uint64_t* arch_vm_address_space_root(arch_address_space_t address_space);
arch_address_space_t arch_vm_create_user_address_space(void);
arch_address_space_t arch_vm_clone_address_space(arch_address_space_t address_space);
void arch_vm_destroy_user_address_space(arch_address_space_t address_space);
void arch_vm_map_page(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t flags);
void arch_vm_map_range(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags);
uint64_t arch_vm_get_phys(arch_address_space_t address_space, uint64_t vaddr);
void arch_vm_unmap_page(arch_address_space_t address_space, uint64_t vaddr);
void arch_vm_update_page_flags(arch_address_space_t address_space, uint64_t vaddr, uint64_t new_flags);

#endif
