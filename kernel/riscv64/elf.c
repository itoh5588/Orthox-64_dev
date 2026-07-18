#include <stdint.h>
#include "riscv64/elf.h"
#include "riscv64/vm.h"

#define RISCV64_PAGE_SIZE 4096ULL
#define RISCV64_PAGE_MASK (~(RISCV64_PAGE_SIZE - 1ULL))

static void riscv64_memcpy(void* dst, const void* src, uint64_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (size--) *d++ = *s++;
}

int riscv64_elf_load_segment_bootstrap(uint64_t address_space,
                                       uint64_t vaddr,
                                       const void* src,
                                       uint64_t filesz,
                                       uint64_t memsz,
                                       uint64_t flags) {
    uint64_t curr = 0;
    const uint8_t* bytes = (const uint8_t*)src;
    if (!address_space || !memsz) return -1;

    while (curr < memsz) {
        uint64_t seg_vaddr = vaddr + curr;
        uint64_t page_base = seg_vaddr & RISCV64_PAGE_MASK;
        uint64_t page_off = seg_vaddr - page_base;
        uint64_t chunk = RISCV64_PAGE_SIZE - page_off;
        uint64_t phys = riscv64_vm_get_phys(address_space, page_base);
        if (curr + chunk > memsz) chunk = memsz - curr;

        if (!phys) {
            phys = riscv64_vm_bootstrap_alloc_page();
            if (!phys) return -1;
            riscv64_vm_map_page(address_space, page_base, phys, flags);
        }

        if (curr < filesz) {
            uint64_t copy = filesz - curr;
            if (copy > chunk) copy = chunk;
            riscv64_memcpy((void*)(uintptr_t)(phys + page_off), bytes + curr, copy);
        }

        curr += chunk;
    }

    return 0;
}
