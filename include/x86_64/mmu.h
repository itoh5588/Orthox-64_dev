#ifndef ORTHOX_ARCH_MMU_H
#define ORTHOX_ARCH_MMU_H

#include <stdint.h>

static inline uint64_t arch_mmu_read_address_space(void) {
    uint64_t address_space;
    __asm__ volatile("mov %%cr3, %0" : "=r"(address_space));
    return address_space;
}

static inline void arch_mmu_write_address_space(uint64_t address_space) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(address_space) : "memory");
}

static inline void arch_mmu_reload_address_space(void) {
    arch_mmu_write_address_space(arch_mmu_read_address_space());
}

static inline void arch_mmu_invalidate_page(uint64_t vaddr) {
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

#endif
