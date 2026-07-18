#ifndef ORTHOX_ARCH_VM_BACKEND_H
#define ORTHOX_ARCH_VM_BACKEND_H

#include <stdint.h>
#include "x86_64/trap.h"
#include "x86_64/vm.h"

void arch_vm_backend_init(uint64_t* kernel_root);
void arch_vm_backend_map_page(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t flags);
void arch_vm_backend_map_range(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags);
uint64_t arch_vm_backend_get_phys(uint64_t* root, uint64_t vaddr);
arch_address_space_t arch_vm_backend_clone_address_space(uint64_t* old_root);
void arch_vm_backend_destroy_user_address_space(arch_address_space_t address_space);
void arch_vm_backend_unmap_page(uint64_t* root, uint64_t vaddr);
void arch_vm_backend_update_page_flags(uint64_t* root, uint64_t vaddr, uint64_t new_flags);
void arch_vm_backend_handle_page_fault(arch_interrupt_frame_t* frame);

#endif
