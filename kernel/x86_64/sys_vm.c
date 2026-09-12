#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "pmm.h"
#include "sys_internal.h"
#include "task.h"
#include "vmm.h"

#ifndef ORTHOX_MEM_TRACE
#define ORTHOX_MEM_TRACE 0
#endif

#define MMAP_BASE_ADDR 0x0000200000000000ULL
#define MMAP_TOP_ADDR  0x00007F0000000000ULL
#define USER_TOP_ADDR  0x0000800000000000ULL

extern void puts(const char* s);
extern void puthex(uint64_t v);

void sys_brk_init(uint64_t initial_break) {
    (void)initial_break;
}

static void* kernel_memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}


#if ORTHOX_MEM_TRACE
static int kernel_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int memtrace_current_enabled(void) {
    struct task* current = get_current_task();
    return current && kernel_streq(current->comm, "cc1");
}

static void memtrace_prefix(const char* tag) {
    struct task* current = get_current_task();
    puts("[memtrace] ");
    puts(tag);
    puts(" pid=0x"); puthex(current ? (uint64_t)current->pid : 0);
    puts(" brk=0x"); puthex(current ? current->heap_break : 0);
    puts(" mmap_end=0x"); puthex(current ? current->mmap_end : 0);
    puts(" pmm_alloc=0x"); puthex(pmm_get_allocated_pages());
    puts(" pmm_free=0x"); puthex(pmm_get_free_pages());
}
#endif

static void memtrace_brk(uint64_t requested, uint64_t old_brk, uint64_t new_brk, uint64_t pages) {
#if ORTHOX_MEM_TRACE
    if (!memtrace_current_enabled()) return;
    memtrace_prefix("brk");
    puts(" req=0x"); puthex(requested);
    puts(" old=0x"); puthex(old_brk);
    puts(" new=0x"); puthex(new_brk);
    puts(" pages=0x"); puthex(pages);
    puts("\r\n");
#else
    (void)requested; (void)old_brk; (void)new_brk; (void)pages;
#endif
}







void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd, int64_t offset);
int sys_munmap(void* addr, size_t length);

uint64_t sys_brk(uint64_t addr) {
    struct task* current = get_current_task();
    uint64_t old_break;
    uint64_t pages = 0;
    if (!current) return 0;
    old_break = current->heap_break;
    // Refuse breaks that would run into the mmap region; the caller sees the
    // unchanged break, which is how brk() reports failure.
    if (addr == 0 || addr <= current->heap_break || addr >= MMAP_BASE_ADDR) {
        memtrace_brk(addr, old_break, current->heap_break, 0);
        return current->heap_break;
    }
    uint64_t current_page = (current->heap_break + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    uint64_t target_page = (addr + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    while (current_page < target_page) {
        void* phys_mem = pmm_alloc(1);
        if (!phys_mem) {
            puts("[sys_brk] pmm_alloc failed!\r\n");
            return current->heap_break;
        }
        kernel_memset(PHYS_TO_VIRT(phys_mem), 0, PAGE_SIZE);
        vmm_map_page(pml4, current_page, (uint64_t)phys_mem, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        current_page += PAGE_SIZE;
        pages++;
    }
    current->heap_break = addr;
    memtrace_brk(addr, old_break, current->heap_break, pages);
    return current->heap_break;
}

int sys_madvise(void* addr, size_t len, int advice) {
    (void)addr;
    (void)len;
    (void)advice;
    return 0;
}

/* **sys_mprotect は kernel/sys_mmap.c へ移した (2026-09-12)。**
 * COW を保つ部分は include/x86_64/vm.h の arch_vm_protect_page に移してある
 * (aarch64 / riscv64 は fork でページを写すので COW が無い) */

/* **sys_mremap は kernel/sys_mmap.c へ移した (2026-09-12)。**
 * 保護を引き継ぐ部分は include/x86_64/vm.h の arch_vm_get_page_prot に
 * 移してある (COW のページを「書けた」と読む判断もあちら) */

/* **sys_mmap / sys_munmap は kernel/sys_mmap.c へ移した (2026-09-11)。**
 * aarch64 / riscv64 の linux_syscall.c 側と 2 実装あったものを 1 つにした。
 * 同じファイルに残っている mremap / mprotect / brk はまだ x86 だけの実装 */
