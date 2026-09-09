#ifndef ORTHOX_ARCH_RISCV64_ELF_H
#define ORTHOX_ARCH_RISCV64_ELF_H

#include <stdint.h>

int riscv64_elf_load_segment_bootstrap(uint64_t address_space,
                                       uint64_t vaddr,
                                       const void* src,
                                       uint64_t filesz,
                                       uint64_t memsz,
                                       uint64_t flags);

#endif
