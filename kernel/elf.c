#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "elf64.h"
#include "pmm.h"
#include "vmm.h"
#include "limine.h"

extern void puts(const char *s);
extern void puthex(uint64_t v);

static int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

static void* kernel_memcpy(void* dest, const void* src, size_t n) {
    char* d = dest;
    const char* s = src;
    while (n--) *d++ = *s++;
    return dest;
}

static void* kernel_memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

// ELF フラグから VMM フラグへの変換
static uint64_t elf_flags_to_vmm(uint32_t p_flags) {
    uint64_t vmm_flags = PTE_PRESENT | PTE_USER;
    if (p_flags & PF_W) vmm_flags |= PTE_WRITABLE;
    // NX ビットの使用を一時停止
    // if (!(p_flags & PF_X)) vmm_flags |= PTE_NX;
    return vmm_flags;
}

// ページエントリのフラグを更新（既存の権限とマージ）
static void update_page_flags(uint64_t* pml4, uint64_t vaddr, uint64_t new_flags) {
    uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT(pml4[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    uint64_t* pd  = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    uint64_t* pt  = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    uint64_t* pte = &pt[PT_IDX(vaddr)];

    // 既存のフラグとマージ (WRITABLE は OR, NX は AND)
    uint64_t merged = *pte | (new_flags & PTE_WRITABLE);
    if (!(new_flags & PTE_NX)) {
        merged &= ~PTE_NX; // 一箇所でも実行許可があれば実行可能にする
    }
    *pte = merged;
}

struct elf_info elf_load(uint64_t* pml4, void* elf_data, uint64_t load_bias) {
    struct elf_info info;
    kernel_memset(&info, 0, sizeof(info));
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_data;

    if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0) {
        puts("ELF: Invalid magic\r\n");
        return info;
    }

    info.type = ehdr->e_type;
    info.load_bias = load_bias;
    info.entry = (void*)(ehdr->e_entry + load_bias);
    info.phent = ehdr->e_phentsize;
    info.phnum = ehdr->e_phnum;
    Elf64_Phdr* phdr = (Elf64_Phdr*)((uint8_t*)elf_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        if (ehdr->e_phoff < phdr[i].p_offset) continue;
        uint64_t phdr_end = ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize;
        if (phdr_end > phdr[i].p_offset + phdr[i].p_filesz) continue;
        info.phdr_vaddr = phdr[i].p_vaddr + (ehdr->e_phoff - phdr[i].p_offset) + load_bias;
        break;
    }

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_INTERP) {
            info.has_interp = true;
            uint64_t len = phdr[i].p_filesz;
            if (len > sizeof(info.interp_path) - 1) len = sizeof(info.interp_path) - 1;
            kernel_memcpy(info.interp_path, (uint8_t*)elf_data + phdr[i].p_offset, len);
            info.interp_path[len] = '\0';
        }

        if (phdr[i].p_type == PT_TLS) {
            info.tls_vaddr = phdr[i].p_vaddr + load_bias;
            info.tls_filesz = phdr[i].p_filesz;
            info.tls_memsz = phdr[i].p_memsz;
            info.tls_align = phdr[i].p_align;
        }

        if (phdr[i].p_type == PT_LOAD) {
            uint64_t vaddr_start = phdr[i].p_vaddr + load_bias;
            uint64_t vaddr_end = vaddr_start + phdr[i].p_memsz;
            uint64_t filesz = phdr[i].p_filesz;
            uint64_t offset = phdr[i].p_offset;
            uint64_t vmm_flags = elf_flags_to_vmm(phdr[i].p_flags);

            if (vaddr_end > info.max_vaddr) {
                info.max_vaddr = vaddr_end;
            }

            // ページごとにループしてロード
            uint64_t curr_vaddr = vaddr_start;
            while (curr_vaddr < vaddr_end) {
                uint64_t page_base = curr_vaddr & ~(PAGE_SIZE - 1);
                uint64_t phys_addr = vmm_get_phys(pml4, page_base);

                if (phys_addr == 0) {
                    // まだマップされていないページ
                    void* new_page = pmm_alloc(1);
                    if (!new_page) {
                        puts("ELF: PMM alloc failed\r\n");
                        return info;
                    }
                    kernel_memset(PHYS_TO_VIRT(new_page), 0, PAGE_SIZE);
                    // 最初は現在のセグメントの権限でマップ
                    vmm_map_page(pml4, page_base, (uint64_t)new_page, vmm_flags);
                    phys_addr = (uint64_t)new_page;
                } else {
                    // 既にマップされている場合は権限をマージ
                    update_page_flags(pml4, page_base, vmm_flags);
                }

                // コピー範囲の計算
                uint64_t offset_in_page = curr_vaddr - page_base;
                uint64_t size_in_page = PAGE_SIZE - offset_in_page;
                if (curr_vaddr + size_in_page > vaddr_end) {
                    size_in_page = vaddr_end - curr_vaddr;
                }

                // ファイルからのデータコピー
                if (curr_vaddr < vaddr_start + filesz) {
                    uint64_t bytes_to_copy = (vaddr_start + filesz) - curr_vaddr;
                    if (bytes_to_copy > size_in_page) {
                        bytes_to_copy = size_in_page;
                    }
                    
                    uint8_t* src = (uint8_t*)elf_data + offset + (curr_vaddr - vaddr_start);
                    kernel_memcpy((uint8_t*)PHYS_TO_VIRT(phys_addr) + offset_in_page, src, bytes_to_copy);
                }

                curr_vaddr += size_in_page;
            }
        }
    }

    return info;
}
