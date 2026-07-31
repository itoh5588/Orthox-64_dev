#include <stddef.h>
#include <stdint.h>
#include "pmm.h"
#include "riscv64/boot.h"
#include "riscv64/vm.h"
#include "spinlock.h"
#include "vmm.h"

extern char __kernel_end[];

#define RISCV64_PMM_MAX_PAGES 131072U

static uint8_t g_riscv64_pmm_bitmap[(RISCV64_PMM_MAX_PAGES + 7U) / 8U];
static uint16_t g_riscv64_pmm_refcounts[RISCV64_PMM_MAX_PAGES];
static uint64_t g_riscv64_pmm_base;
static uint64_t g_riscv64_pmm_pages;
static void* g_riscv64_isa_dma_page;
/*
 * ビットマップと refcount を守るロック。無いと複数 hart の pmm_alloc() が
 * 同じページを 2 度配ってしまう (ビットマップ更新が read-modify-write
 * なので、同じバイトに当たる別ページの取り合いでも壊れる)。
 * SMP=4 でアドレス空間破棄中の load page fault
 * (riscv64_vm_destroy_table が壊れた PTE を辿る) として顕在化した。
 * 静的変数のゼロ初期化 = spinlock_init 済み。
 */
static spinlock_t g_riscv64_pmm_lock;

static inline void riscv64_pmm_set(uint64_t page) {
    g_riscv64_pmm_bitmap[page / 8U] |= (uint8_t)(1U << (page % 8U));
}

static inline void riscv64_pmm_clear(uint64_t page) {
    g_riscv64_pmm_bitmap[page / 8U] &= (uint8_t)~(1U << (page % 8U));
}

static inline int riscv64_pmm_test(uint64_t page) {
    return (g_riscv64_pmm_bitmap[page / 8U] >> (page % 8U)) & 1U;
}

static uint64_t riscv64_align_up_page(uint64_t value) {
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

void pmm_init(void) {
    const riscv64_boot_info_t* boot = riscv64_boot_info();
    uint64_t mem_base;
    uint64_t mem_end;
    uint64_t free_base;

    g_hhdm_offset = 0;
    g_riscv64_isa_dma_page = 0;
    g_riscv64_pmm_base = 0;
    g_riscv64_pmm_pages = 0;

    for (uint64_t i = 0; i < sizeof(g_riscv64_pmm_bitmap); i++) g_riscv64_pmm_bitmap[i] = 0xffU;
    for (uint64_t i = 0; i < RISCV64_PMM_MAX_PAGES; i++) g_riscv64_pmm_refcounts[i] = 0;

    if (!boot || boot->memory_size == 0) return;

    mem_base = riscv64_align_up_page(boot->memory_base);
    mem_end = boot->memory_base + boot->memory_size;
    free_base = riscv64_align_up_page((uint64_t)(uintptr_t)__kernel_end);
    if (free_base < mem_base) free_base = mem_base;
    if (free_base >= mem_end) return;

    g_riscv64_pmm_base = free_base;
    g_riscv64_pmm_pages = (mem_end - free_base) / PAGE_SIZE;
    if (g_riscv64_pmm_pages > RISCV64_PMM_MAX_PAGES) {
        g_riscv64_pmm_pages = RISCV64_PMM_MAX_PAGES;
    }

    for (uint64_t page = 0; page < g_riscv64_pmm_pages; page++) {
        riscv64_pmm_clear(page);
    }

    // DTB が管理領域内にある場合はそのページを予約扱いにする
    if (boot->dtb_size != 0 && boot->dtb_pa + boot->dtb_size > free_base && boot->dtb_pa < mem_end) {
        uint64_t dtb_start = boot->dtb_pa & ~(PAGE_SIZE - 1ULL);
        uint64_t dtb_end = riscv64_align_up_page(boot->dtb_pa + boot->dtb_size);
        if (dtb_start < free_base) dtb_start = free_base;
        for (uint64_t pa = dtb_start; pa < dtb_end && pa < mem_end; pa += PAGE_SIZE) {
            uint64_t page = (pa - free_base) / PAGE_SIZE;
            if (page < g_riscv64_pmm_pages) riscv64_pmm_set(page);
        }
    }
}

void* pmm_alloc(size_t pages) {
    uint64_t run = 0;
    uint64_t start = 0;
    void* result = 0;
    uint64_t flags;

    if (pages == 0 || g_riscv64_pmm_pages == 0) return 0;

    flags = spin_lock_irqsave(&g_riscv64_pmm_lock);
    for (uint64_t i = 0; i < g_riscv64_pmm_pages; i++) {
        if (!riscv64_pmm_test(i)) {
            if (run == 0) start = i;
            run++;
            if (run == pages) {
                for (uint64_t j = 0; j < pages; j++) {
                    uint64_t page = start + j;
                    riscv64_pmm_set(page);
                    g_riscv64_pmm_refcounts[page] = 1;
                }
                result = (void*)(uintptr_t)(g_riscv64_pmm_base + start * PAGE_SIZE);
                break;
            }
        } else {
            run = 0;
        }
    }
    spin_unlock_irqrestore(&g_riscv64_pmm_lock, flags);

    return result;
}

void pmm_free(void* addr, size_t pages) {
    uint64_t base = (uint64_t)(uintptr_t)addr;
    uint64_t flags;
    if (!addr || pages == 0 || base < g_riscv64_pmm_base) return;
    flags = spin_lock_irqsave(&g_riscv64_pmm_lock);
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = base + i * PAGE_SIZE;
        uint64_t page = (phys - g_riscv64_pmm_base) / PAGE_SIZE;
        if (page >= g_riscv64_pmm_pages) break;
        if (g_riscv64_pmm_refcounts[page] > 0) {
            g_riscv64_pmm_refcounts[page]--;
            if (g_riscv64_pmm_refcounts[page] == 0) {
                riscv64_pmm_clear(page);
            }
        }
    }
    spin_unlock_irqrestore(&g_riscv64_pmm_lock, flags);
}

void pmm_incref(void* addr) {
    uint64_t phys = (uint64_t)(uintptr_t)addr;
    uint64_t page;
    uint64_t flags;
    if (!addr || phys < g_riscv64_pmm_base) return;
    page = (phys - g_riscv64_pmm_base) / PAGE_SIZE;
    if (page >= g_riscv64_pmm_pages) return;
    flags = spin_lock_irqsave(&g_riscv64_pmm_lock);
    g_riscv64_pmm_refcounts[page]++;
    spin_unlock_irqrestore(&g_riscv64_pmm_lock, flags);
}

uint16_t pmm_get_ref(void* addr) {
    uint64_t phys = (uint64_t)(uintptr_t)addr;
    uint64_t page;
    uint64_t flags;
    uint16_t ref;
    if (!addr || phys < g_riscv64_pmm_base) return 0;
    page = (phys - g_riscv64_pmm_base) / PAGE_SIZE;
    if (page >= g_riscv64_pmm_pages) return 0;
    flags = spin_lock_irqsave(&g_riscv64_pmm_lock);
    ref = g_riscv64_pmm_refcounts[page];
    spin_unlock_irqrestore(&g_riscv64_pmm_lock, flags);
    return ref;
}

void* pmm_get_isa_dma_page(void) {
    return g_riscv64_isa_dma_page;
}
