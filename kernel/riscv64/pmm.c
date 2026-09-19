#include <stddef.h>
#include <stdint.h>
#include "pmm.h"
#include "pmm_core.h"
#include "riscv64/boot.h"
#include "riscv64/vm.h"
#include "vmm.h"

/* 配り方は共通層 (kernel/pmm_core.c)。ここは管理する範囲と管理情報の
 * 置き場を決めるだけ (include/pmm_core.h)。
 *
 * **以前はここにビットマップ確保があり、毎回ページ 0 から 1 ビットずつ
 * 探していた。**QEMU virt (512MB) の forkbench で 1 回の確保あたり平均
 * 1,965 ページを走査し、使用中が増えるほど伸びる作りだった (2026-09-19) */

extern char __kernel_end[];

#define RISCV64_PMM_MAX_PAGES 131072U

/* 管理情報は静的配列。riscv64 のカーネルは恒等マッピングで走るので、
 * 配列の番地がそのまま物理アドレスになる (arch_pmm_meta_ptr) */
static uint8_t g_riscv64_pmm_bitmap[(RISCV64_PMM_MAX_PAGES + 7U) / 8U];
static uint16_t g_riscv64_pmm_refcounts[RISCV64_PMM_MAX_PAGES];

static uint64_t riscv64_align_up_page(uint64_t value) {
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

void* arch_pmm_meta_ptr(uint64_t pa) {
    return (void*)(uintptr_t)pa;
}

/* 0 埋めは呼ぶ側 (以前の pmm_alloc も埋めていなかった) */
void arch_pmm_after_alloc(uint64_t pa, uint64_t pages) {
    (void)pa;
    (void)pages;
}

void pmm_init(void) {
    const riscv64_boot_info_t* boot = riscv64_boot_info();
    uint64_t mem_base;
    uint64_t mem_end;
    uint64_t free_base;
    uint64_t pages;

    g_hhdm_offset = 0;
    pmm_core_setup(0, 0, (uint64_t)(uintptr_t)g_riscv64_pmm_bitmap,
                   (uint64_t)(uintptr_t)g_riscv64_pmm_refcounts);

    if (!boot || boot->memory_size == 0) return;

    mem_base = riscv64_align_up_page(boot->memory_base);
    mem_end = boot->memory_base + boot->memory_size;
    free_base = riscv64_align_up_page((uint64_t)(uintptr_t)__kernel_end);
    if (free_base < mem_base) free_base = mem_base;
    if (free_base >= mem_end) return;

    pages = (mem_end - free_base) / PAGE_SIZE;
    if (pages > RISCV64_PMM_MAX_PAGES) pages = RISCV64_PMM_MAX_PAGES;

    pmm_core_setup(free_base, pages, (uint64_t)(uintptr_t)g_riscv64_pmm_bitmap,
                   (uint64_t)(uintptr_t)g_riscv64_pmm_refcounts);
    pmm_core_mark_free(free_base, pages * PAGE_SIZE);

    // DTB が管理領域内にある場合はそのページを予約扱いにする
    if (boot->dtb_size != 0 && boot->dtb_pa + boot->dtb_size > free_base && boot->dtb_pa < mem_end) {
        pmm_core_mark_used(boot->dtb_pa, boot->dtb_size);
    }
}

/* ISA DMA は x86 の話。riscv64 では使わない */
void* pmm_get_isa_dma_page(void) {
    return 0;
}
