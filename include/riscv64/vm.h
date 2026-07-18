#ifndef ORTHOX_ARCH_RISCV64_VM_H
#define ORTHOX_ARCH_RISCV64_VM_H

#include <stdint.h>

#define RISCV64_VM_PAGE_R   (1ULL << 0)
#define RISCV64_VM_PAGE_W   (1ULL << 1)
#define RISCV64_VM_PAGE_X   (1ULL << 2)
#define RISCV64_VM_PAGE_G   (1ULL << 3)
#define RISCV64_VM_PAGE_U   (1ULL << 4)

typedef uint64_t arch_address_space_t;

void riscv64_vm_init(void);
void riscv64_vm_dump_plan(void);
uint64_t riscv64_vm_kernel_address_space(void);
uint64_t riscv64_vm_root_pa(void);
uint64_t riscv64_vm_current_address_space(void);
void riscv64_vm_activate_address_space(uint64_t root_pa);
uint64_t riscv64_vm_create_address_space(void);
uint64_t riscv64_vm_clone_kernel_address_space(void);
void riscv64_vm_destroy_address_space(uint64_t root_pa);
uint64_t riscv64_vm_bootstrap_alloc_page(void);
void riscv64_vm_bootstrap_free_page(uint64_t phys_addr);
void riscv64_vm_memcpy_page(uint64_t dst_phys, uint64_t src_phys);
void riscv64_vm_map_page(uint64_t root_pa, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
void riscv64_vm_map_range(uint64_t root_pa, uint64_t virt_addr, uint64_t phys_addr, uint64_t size, uint64_t flags);
uint64_t riscv64_vm_get_phys(uint64_t root_pa, uint64_t virt_addr);
void riscv64_vm_unmap_page(uint64_t root_pa, uint64_t virt_addr);
void riscv64_vm_update_page_flags(uint64_t root_pa, uint64_t virt_addr, uint64_t flags);

arch_address_space_t arch_vm_kernel_address_space(void);
arch_address_space_t arch_vm_create_user_address_space(void);
arch_address_space_t arch_vm_clone_address_space(arch_address_space_t address_space);
void arch_vm_destroy_user_address_space(arch_address_space_t address_space);
void arch_vm_map_page(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t flags);
void arch_vm_map_range(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags);
uint64_t arch_vm_get_phys(arch_address_space_t address_space, uint64_t vaddr);
void arch_vm_unmap_page(arch_address_space_t address_space, uint64_t vaddr);
void arch_vm_update_page_flags(arch_address_space_t address_space, uint64_t vaddr, uint64_t flags);

#endif
