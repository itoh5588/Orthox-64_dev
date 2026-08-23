#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "elf64.h"
#include "pmm.h"
#include "vmm.h"  /* PHYS_TO_VIRT/VIRT_TO_PHYS, kept for HHDM */
#include "arch_vm.h"
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
    return arch_vm_user_page_flags((p_flags & PF_W) != 0, (p_flags & PF_X) != 0);
}

// ページエントリのフラグを更新（arch_vm 経由で既存の権限とマージ）
static void update_page_flags(arch_address_space_t address_space, uint64_t vaddr, uint64_t new_flags) {
    arch_vm_update_page_flags(address_space, vaddr, new_flags);
}

struct elf_info elf_load(arch_address_space_t address_space, void* elf_data, uint64_t load_bias) {
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
                uint64_t phys_addr = arch_vm_get_phys(address_space, page_base);

                if (phys_addr == 0) {
                    // まだマップされていないページ
                    void* new_page = pmm_alloc(1);
                    if (!new_page) {
                        puts("ELF: PMM alloc failed\r\n");
                        return info;
                    }
                    kernel_memset(PHYS_TO_VIRT(new_page), 0, PAGE_SIZE);
                    // 最初は現在のセグメントの権限でマップ
                    arch_vm_map_page(address_space, page_base, (uint64_t)new_page, vmm_flags);
                    phys_addr = (uint64_t)new_page;
                } else {
                    // 既にマップされている場合は権限をマージ
                    update_page_flags(address_space, page_base, vmm_flags);
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

                /* **S-3: 命令になるページは I-cache と揃えてから離れる。**
                 *
                 * ここまでは HHDM (Normal WB) への素の memcpy なので、
                 * 書いたバイトは D-cache に居るだけかもしれず、しかも
                 * この物理ページを前に使っていたプログラムの古い I-cache 行が
                 * 残っている。**A72 の I-cache は PIPT で D-cache を
                 * スヌープしない**ので、どちらも命令フェッチに漏れる。
                 *
                 * **ページまるごと揃える。** memset した部分 (bss) も
                 * 実行可能セグメントなら命令として読まれうる。
                 *
                 * 実行しないセグメントは飛ばす。.data / .bss を毎回舐めると
                 * 大きなバイナリで無駄が出る */
                if (phdr[i].p_flags & PF_X) {
                    arch_sync_icache_range((void*)PHYS_TO_VIRT(phys_addr), PAGE_SIZE);
                }

                curr_vaddr += size_in_page;
            }
        }
    }

    return info;
}
