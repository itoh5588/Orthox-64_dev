#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "elf64.h"
#include "pmm.h"
#include "vmm.h"  /* PHYS_TO_VIRT/VIRT_TO_PHYS, kept for HHDM */
#include "arch_vm.h"

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

/* **1 ページを覆う PT_LOAD 全部の権限を束ねる (2026-09-12)。**
 *
 * 段の境目は同じページに乗りうる (リンカが -z noseparate-code 相当で詰めた
 * 場合)。そのページの権限は**和**でなければならない —— .text (R+X) と
 * .data (R+W) が同居していたら、
 *
 *   後勝ち  R+W になり、**そのページの命令が実行できない**
 *   先勝ち  R+X になり、**そのページのデータに書けない**
 *
 * どちらも動かない。和 (R+W+X) だけが動く。
 *
 * **和は ELF の段階で取る。**arch_vm_update_page_flags に「マージしろ」と
 * 言わせると、x86 の PTE_NX / aarch64 の AP[2]・UXN が**負のビット**なので
 * ビット OR では正しくならず、3 アーキそれぞれで極性を間違えうる
 * (2026-09-11 §3 の「x86 だけ NX を立てていなかった」と同じ形)。PF_W / PF_X
 * は 3 アーキ共通の正のビットなので、ここで束ねれば 1 箇所で済む。
 *
 * 段の数は 10 前後なので、ページごとに全段を見ても、同じループが既にやって
 * いる 4KB の memset / memcpy に比べれば無視できる。phdr の並び順にも
 * 依存しない。
 *
 * **現に読んでいる ELF に跨ぎは無い** (out/ と ports/ の全 ELF を調べた。
 * 跨ぐのはカーネル自身の ELF だけで、あれは elf.c が読むものではない)。
 * 起きたときに初めて壊れる類なので、起きない側に倒しておく。 */
static uint64_t elf_page_vmm_flags(Elf64_Phdr* phdr, uint16_t phnum,
                                   uint64_t page_base, uint64_t load_bias) {
    uint32_t merged = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t start, end;
        if (phdr[i].p_type != PT_LOAD) continue;
        if (phdr[i].p_memsz == 0) continue;
        start = phdr[i].p_vaddr + load_bias;
        end = start + phdr[i].p_memsz;
        if (end <= page_base || start >= page_base + PAGE_SIZE) continue;
        merged |= phdr[i].p_flags;
    }
    return elf_flags_to_vmm(merged);
}

/* ページエントリのフラグを更新する。**hook は置き換える** (名前のとおり。
 * aarch64 / riscv64 の実装がそう。x86 は 2026-09-12 まで何もしないスタブで、
 * **3 アーキが 3 通りに振る舞っていた** —— x86 は先に触った段の権限が残り、
 * 他の 2 つは後の段で上書きされた)。和は呼ぶ側 (elf_page_vmm_flags) で
 * 取ってあるので、置き換えで正しい */
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
                    // まだマップされていないページ。この段の権限で貼る
                    void* new_page = pmm_alloc(1);
                    if (!new_page) {
                        puts("ELF: PMM alloc failed\r\n");
                        return info;
                    }
                    kernel_memset(PHYS_TO_VIRT(new_page), 0, PAGE_SIZE);
                    arch_vm_map_page(address_space, page_base, (uint64_t)new_page, vmm_flags);
                    phys_addr = (uint64_t)new_page;
                } else {
                    /* **既に貼られている = 段がこのページを跨いだ。**
                     * このページを覆う段全部の権限の和に置き換える。
                     * 和は段の処理順に依らないので、最後に覆った段を
                     * 処理し終えた時点で必ず和になっている */
                    update_page_flags(address_space, page_base,
                                      elf_page_vmm_flags(phdr, ehdr->e_phnum,
                                                         page_base, load_bias));
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
