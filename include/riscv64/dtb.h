#ifndef ORTHOX_ARCH_RISCV64_DTB_H
#define ORTHOX_ARCH_RISCV64_DTB_H

#include <stdint.h>

#define RISCV64_FDT_MAGIC 0xd00dfeedU

#define RISCV64_FDT_BEGIN_NODE  0x00000001U
#define RISCV64_FDT_END_NODE    0x00000002U
#define RISCV64_FDT_PROP        0x00000003U
#define RISCV64_FDT_NOP         0x00000004U
#define RISCV64_FDT_END         0x00000009U

typedef struct riscv64_fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} riscv64_fdt_header_t;

uint32_t riscv64_dtb_read_be32(const void* addr);
uint64_t riscv64_dtb_read_be64(const void* addr);
int riscv64_dtb_valid(uint64_t dtb_pa);
uint32_t riscv64_dtb_total_size(uint64_t dtb_pa);

#endif
