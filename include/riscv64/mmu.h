#ifndef ORTHOX_ARCH_RISCV64_MMU_H
#define ORTHOX_ARCH_RISCV64_MMU_H

#include <stdint.h>
#include "riscv64/csr.h"

#define RISCV64_SATP_MODE_SV39 (8ULL << 60)

static inline uint64_t arch_mmu_read_address_space(void) {
    uint64_t satp = riscv64_read_satp();
    return (satp & ((1ULL << 44) - 1)) << 12;
}

static inline void arch_mmu_write_address_space(uint64_t address_space) {
    uint64_t satp = RISCV64_SATP_MODE_SV39 | (address_space >> 12);
    riscv64_write_satp(satp);
    riscv64_sfence_vma();
}

static inline void arch_mmu_reload_address_space(void) {
    riscv64_sfence_vma();
}

static inline void arch_mmu_invalidate_page(uint64_t vaddr) {
    (void)vaddr;
    riscv64_sfence_vma();
}

#endif
