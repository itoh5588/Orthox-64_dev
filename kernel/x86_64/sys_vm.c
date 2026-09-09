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

static void* kernel_memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    while (n--) *d++ = *s++;
    return dst;
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

static uint64_t align_up_page(uint64_t v) {
    return (v + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static int is_user_mmap_range_valid(uint64_t vaddr, uint64_t size) {
    if (size == 0) return 0;
    if ((vaddr & (PAGE_SIZE - 1)) != 0) return 0;
    if (vaddr < MMAP_BASE_ADDR) return 0;
    if (vaddr >= MMAP_TOP_ADDR) return 0;
    if (vaddr + size < vaddr) return 0;
    if (vaddr + size > MMAP_TOP_ADDR) return 0;
    return 1;
}

static int is_user_page_range_valid(uint64_t vaddr, uint64_t size) {
    if (size == 0) return 0;
    if ((vaddr & (PAGE_SIZE - 1)) != 0) return 0;
    if (vaddr < PAGE_SIZE) return 0;
    if (vaddr >= USER_TOP_ADDR) return 0;
    if (vaddr + size < vaddr) return 0;
    if (vaddr + size > USER_TOP_ADDR) return 0;
    return 1;
}

static int is_range_unmapped(uint64_t* pml4, uint64_t vaddr, uint64_t size) {
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        if (vmm_get_phys(pml4, vaddr + off) != 0) {
            return 0;
        }
    }
    return 1;
}

static uint64_t find_mmap_gap_from(uint64_t* pml4, uint64_t size, uint64_t start, uint64_t end) {
    if (start < MMAP_BASE_ADDR) start = MMAP_BASE_ADDR;
    start = align_up_page(start);
    for (uint64_t base = start; base + size <= end; base += PAGE_SIZE) {
        if (is_range_unmapped(pml4, base, size)) return base;
    }
    return 0;
}

static uint64_t find_mmap_gap(uint64_t* pml4, uint64_t size, uint64_t hint) {
    if (!pml4 || size == 0 || size > (MMAP_TOP_ADDR - MMAP_BASE_ADDR)) return 0;
    if (hint < MMAP_BASE_ADDR || hint >= MMAP_TOP_ADDR) hint = MMAP_BASE_ADDR;

    uint64_t base = find_mmap_gap_from(pml4, size, hint, MMAP_TOP_ADDR);
    if (base != 0) return base;
    if (hint > MMAP_BASE_ADDR) return find_mmap_gap_from(pml4, size, MMAP_BASE_ADDR, hint);
    return 0;
}

static void unmap_one_page(uint64_t* pml4, uint64_t vaddr) {
    if (!(pml4[PML4_IDX(vaddr)] & PTE_PRESENT)) return;
    uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT(pml4[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pdp[PDP_IDX(vaddr)] & PTE_PRESENT)) return;
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pd[PD_IDX(vaddr)] & PTE_PRESENT)) return;
    if (pd[PD_IDX(vaddr)] & PTE_HUGE) return; // mmap() does not create huge pages here.
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    uint64_t* pte = &pt[PT_IDX(vaddr)];
    if (!(*pte & PTE_PRESENT)) return;

    void* page_phys = (void*)(*pte & PTE_ADDR_MASK);
    *pte = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    pmm_free(page_phys, 1);
}

static uint64_t* lookup_user_pte(uint64_t* pml4, uint64_t vaddr) {
    if (!(pml4[PML4_IDX(vaddr)] & PTE_PRESENT)) return 0;
    uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT(pml4[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pdp[PDP_IDX(vaddr)] & PTE_PRESENT)) return 0;
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pd[PD_IDX(vaddr)] & PTE_PRESENT)) return 0;
    if (pd[PD_IDX(vaddr)] & PTE_HUGE) return 0;
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    return &pt[PT_IDX(vaddr)];
}

static void rollback_mmap(uint64_t* pml4, uint64_t base, uint64_t mapped_size) {
    for (uint64_t off = 0; off < mapped_size; off += PAGE_SIZE) {
        unmap_one_page(pml4, base + off);
    }
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

int sys_mprotect(void* addr, size_t length, int prot) {
    struct task* current = get_current_task();
    if (!current || length == 0) return -22;

    uint64_t base = (uint64_t)addr & ~(PAGE_SIZE - 1);
    uint64_t end = align_up_page((uint64_t)addr + (uint64_t)length);
    if (end <= base) return -22;
    if (!is_user_page_range_valid(base, end - base)) return -22;

    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    for (uint64_t vaddr = base; vaddr < end; vaddr += PAGE_SIZE) {
        uint64_t* pte = lookup_user_pte(pml4, vaddr);
        if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) {
            return -12;
        }

        *pte &= ~(PTE_WRITABLE | PTE_NX);
        // COW pages must stay read-only even for PROT_WRITE; the COW fault
        // handler grants write access after resolving the shared page.
        if ((prot & PROT_WRITE) && !(*pte & PTE_COW)) *pte |= PTE_WRITABLE;
        if (!(prot & PROT_EXEC)) *pte |= PTE_NX;
        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    }
    return 0;
}

static void copy_user_range(uint64_t* pml4, uint64_t dst_base, uint64_t src_base, uint64_t length) {
    uint64_t done = 0;
    while (done < length) {
        uint64_t src_v = src_base + done;
        uint64_t dst_v = dst_base + done;
        uint64_t src_off = src_v & (PAGE_SIZE - 1);
        uint64_t dst_off = dst_v & (PAGE_SIZE - 1);
        uint64_t src_phys = vmm_get_phys(pml4, src_v);
        uint64_t dst_phys = vmm_get_phys(pml4, dst_v);

        if (!src_phys || !dst_phys) break;

        uint64_t src_rem = PAGE_SIZE - src_off;
        uint64_t dst_rem = PAGE_SIZE - dst_off;
        uint64_t rem = (src_rem < dst_rem) ? src_rem : dst_rem;
        if (rem > (length - done)) rem = length - done;

        kernel_memcpy((void*)(PHYS_TO_VIRT(dst_phys) + dst_off),
                      (void*)(PHYS_TO_VIRT(src_phys) + src_off), rem);
        done += rem;
    }
}

void* sys_mremap(void* old_addr, size_t old_len, size_t new_len, int flags, void* new_addr) {
    struct task* current = get_current_task();
    uint64_t old_base = (uint64_t)old_addr;
    uint64_t old_size = align_up_page((uint64_t)old_len);
    uint64_t new_size = align_up_page((uint64_t)new_len);
    uint64_t* pml4;
    uint64_t end;
    uint64_t grow_size;
    uint64_t first_phys;
    uint64_t* first_pte;
    int prot = PROT_READ;
    void* mapped;

    if (!current || !old_base || !old_len || !new_len) return (void*)-22;
    if ((old_base & (PAGE_SIZE - 1)) != 0) return (void*)-22;
    if (flags & ~(1 | 2)) return (void*)-22;
    if ((flags & 2) && !(flags & 1)) return (void*)-22;

    pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    if (!is_user_mmap_range_valid(old_base, old_size)) return (void*)-22;
    first_phys = vmm_get_phys(pml4, old_base);
    first_pte = lookup_user_pte(pml4, old_base);
    if (!first_phys || !first_pte) return (void*)-22;
    // A COW page reads as non-writable but was originally writable.
    if (*first_pte & (PTE_WRITABLE | PTE_COW)) prot |= PROT_WRITE;
    if (!(*first_pte & PTE_NX)) prot |= PROT_EXEC;

    if (new_size == old_size) return old_addr;

    if (new_size < old_size) {
        for (uint64_t off = new_size; off < old_size; off += PAGE_SIZE) {
            uint64_t phys = vmm_get_phys(pml4, old_base + off);
            if (phys) {
                unmap_one_page(pml4, old_base + off);
            }
        }
        return old_addr;
    }

    end = old_base + old_size;
    grow_size = new_size - old_size;
    if (is_user_mmap_range_valid(end, grow_size) && is_range_unmapped(pml4, end, grow_size)) {
        for (uint64_t off = 0; off < grow_size; off += PAGE_SIZE) {
            void* phys = pmm_alloc(1);
            if (!phys) return (void*)-12;
            kernel_memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            uint64_t map_flags = PTE_PRESENT | PTE_USER;
            if (prot & PROT_WRITE) map_flags |= PTE_WRITABLE;
            if (!(prot & PROT_EXEC)) map_flags |= PTE_NX;
            vmm_map_page(pml4, end + off, (uint64_t)phys, map_flags);
        }
        return old_addr;
    }

    if (!(flags & 1)) return (void*)-12;

    mapped = sys_mmap((flags & 2) ? new_addr : 0, new_len, prot,
                      MAP_PRIVATE | MAP_ANONYMOUS | ((flags & 2) ? MAP_FIXED : 0), -1, 0);
    if ((int64_t)(uint64_t)mapped < 0) return mapped;

    copy_user_range(pml4, (uint64_t)mapped, old_base, old_size);
    (void)sys_munmap(old_addr, old_len);
    return mapped;
}

static void copy_mmap_file_page(uint8_t* dest, int fd, uint64_t file_off) {
    if (!dest || fd < 0) return;
    int64_t old = fs_lseek(fd, 0, 1);
    if (old < 0) return;
    if (fs_lseek(fd, (int64_t)file_off, 0) >= 0) {
        int64_t n = fs_read(fd, dest, PAGE_SIZE);
        (void)n;
    }
    (void)fs_lseek(fd, old, 0);
}

// File-backed mappings are an eager per-process copy made at map time;
// MAP_SHARED is accepted but behaves like MAP_PRIVATE (no write-back, no
// cross-process visibility).
void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    struct task* current = get_current_task();
    if (!current || length == 0) return (void*)-22;
    if (!(flags & (MAP_PRIVATE | MAP_SHARED))) return (void*)-22;
    if (offset < 0) return (void*)-22;
    if ((offset & (PAGE_SIZE - 1)) != 0) return (void*)-22;

    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    uint64_t size = align_up_page((uint64_t)length);
    if (size == 0) return (void*)-22;

    file_descriptor_t* backing_fd = 0;
    int is_anonymous = (flags & MAP_ANONYMOUS) != 0;
    if (is_anonymous) {
        if (fd != -1 && fd != 0) return (void*)-22;
        if (offset != 0) return (void*)-22;
    } else {
        if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return (void*)-9;
        backing_fd = &current->fds[fd];
        if (fs_fd_type(backing_fd) == FT_PIPE) return (void*)-19;
    }

    uint64_t base = align_up_page((uint64_t)addr);
    if (base == 0 || !(flags & MAP_FIXED)) {
        base = find_mmap_gap(pml4, size, current->mmap_end);
        if (base == 0) return (void*)-12;
        current->mmap_end = align_up_page(base + size);
    } else {
        if ((uint64_t)addr != base) return (void*)-22;
        if (!is_user_mmap_range_valid(base, size)) return (void*)-22;
        if (!is_range_unmapped(pml4, base, size)) {
            // Linux MAP_FIXED replaces overlapping mapping; keep same behavior.
            for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
                unmap_one_page(pml4, base + off);
            }
        }
    }

    if (!is_user_mmap_range_valid(base, size)) return (void*)-22;

    uint64_t map_flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) map_flags |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC)) map_flags |= PTE_NX;

    uint64_t mapped = 0;
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        void* phys = pmm_alloc(1);
        if (!phys) {
            rollback_mmap(pml4, base, mapped);
            return (void*)-12;
        }
        kernel_memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        if (!is_anonymous) {
            copy_mmap_file_page((uint8_t*)PHYS_TO_VIRT(phys), fd, (uint64_t)offset + off);
        }
        vmm_map_page(pml4, base + off, (uint64_t)phys, map_flags);
        mapped += PAGE_SIZE;
    }

    return (void*)base;
}

int sys_munmap(void* addr, size_t length) {
    struct task* current = get_current_task();
    if (!current || length == 0) return -22;

    uint64_t base = (uint64_t)addr;
    if (base & (PAGE_SIZE - 1)) return -22;
    uint64_t size = align_up_page((uint64_t)length);
    if (!is_user_mmap_range_valid(base, size)) return -22;

    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        unmap_one_page(pml4, base + off);
    }
    return 0;
}
