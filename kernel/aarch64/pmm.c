/*
 * 物理メモリ管理 (M3b-2)。ビットマップで 4KB ページを配る。
 *
 * kernel/riscv64/pmm.c と同じ方式だが、あちらは共有ヘッダ (pmm.h / vmm.h /
 * spinlock.h) に繋がっている。aarch64 はまだ共有層に乗っていないので
 * 自己完結にしてある。共有層への接続は M3c。
 *
 * **管理するのは物理アドレス。** カーネルは上位 VA で走っているので、
 * 中身を触るときは aarch64_phys_to_virt を通すこと。
 *
 * 空き領域の始まりに注意:
 *
 *   カーネルイメージ (__kernel_end まで)
 *   ブートスタック   8 CPU x 64KB   ← **ここも使用中。空きにしてはいけない**
 *   ここから空き
 *
 * ブートスタックは start.S がカーネルイメージの直後に取っている
 * (.bss だと .boot から届かず、.boot に置くとイメージが 512KB 太るため)。
 * ここを空きとして配ると、いま自分が乗っているスタックを別の用途に
 * 渡すことになる。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/vm.h"

#define AARCH64_PAGE_SIZE   0x1000ULL
#define AARCH64_BOOT_CPUS   8
#define AARCH64_BOOT_STACK  65536ULL

/* 管理できるページ数の上限。4KB x 131072 = 512MB */
#define AARCH64_PMM_MAX_PAGES 131072U

extern char __kernel_end[];

static uint8_t g_bitmap[(AARCH64_PMM_MAX_PAGES + 7U) / 8U];
static uint64_t g_base_pa;      /* 管理領域の先頭 (物理) */
static uint64_t g_pages;        /* 管理しているページ数 */
static uint64_t g_used;

static void pmm_set(uint64_t page)   { g_bitmap[page / 8U] |= (uint8_t)(1U << (page % 8U)); }
static void pmm_clear(uint64_t page) { g_bitmap[page / 8U] &= (uint8_t)~(1U << (page % 8U)); }
static int  pmm_test(uint64_t page)  { return (g_bitmap[page / 8U] >> (page % 8U)) & 1U; }

static uint64_t align_up_page(uint64_t v) {
    return (v + AARCH64_PAGE_SIZE - 1ULL) & ~(AARCH64_PAGE_SIZE - 1ULL);
}

/* いま持っているポインタが指すものの物理アドレス。MMU を入れる前は
 * シンボルのアドレスがそのまま物理、上位 VA へ移った後は VA になる */
static uint64_t sym_pa(const void* p) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    return aarch64_vm_running_high() ? aarch64_virt_to_phys(a) : a;
}

void aarch64_pmm_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t mem_end, free_base;

    for (uint64_t i = 0; i < sizeof(g_bitmap); i++) g_bitmap[i] = 0xffU;
    g_base_pa = 0;
    g_pages = 0;
    g_used = 0;

    if (!b || b->memory_size == 0) return;

    mem_end = b->memory_base + b->memory_size;

    /* **カーネルイメージとブートスタックの後ろから。** ブートスタックを
     * 空きに含めると、いま乗っているスタックを配ってしまう */
    free_base = align_up_page(sym_pa(__kernel_end)) +
                AARCH64_BOOT_CPUS * AARCH64_BOOT_STACK;
    free_base = align_up_page(free_base);
    if (free_base < b->memory_base) free_base = b->memory_base;
    if (free_base >= mem_end) return;

    g_base_pa = free_base;
    g_pages = (mem_end - free_base) / AARCH64_PAGE_SIZE;
    if (g_pages > AARCH64_PMM_MAX_PAGES) g_pages = AARCH64_PMM_MAX_PAGES;

    for (uint64_t page = 0; page < g_pages; page++) pmm_clear(page);

    /* DTB が管理領域の中にあるなら予約する。QEMU virt では RAM の先頭
     * (カーネルより手前) なので普通は当たらないが、実機では分からない */
    if (b->dtb_size != 0 && b->dtb_pa + b->dtb_size > free_base && b->dtb_pa < mem_end) {
        uint64_t s = b->dtb_pa & ~(AARCH64_PAGE_SIZE - 1ULL);
        uint64_t e = align_up_page(b->dtb_pa + b->dtb_size);
        if (s < free_base) s = free_base;
        for (uint64_t pa = s; pa < e && pa < mem_end; pa += AARCH64_PAGE_SIZE) {
            uint64_t page = (pa - free_base) / AARCH64_PAGE_SIZE;
            if (page < g_pages) { if (!pmm_test(page)) g_used++; pmm_set(page); }
        }
    }
}

/* 連続した pages 枚を確保して**物理アドレス**を返す。0 なら失敗。
 * 中身は 0 で埋める (ページテーブルに使うので、ごみが残っていると
 * 有効ビットが立ったままの descriptor を掴むことになる) */
uint64_t aarch64_pmm_alloc(uint64_t pages) {
    uint64_t run = 0, start = 0;

    if (pages == 0 || g_pages == 0) return 0;

    for (uint64_t page = 0; page < g_pages; page++) {
        if (pmm_test(page)) { run = 0; continue; }
        if (run == 0) start = page;
        if (++run == pages) {
            uint64_t pa = g_base_pa + start * AARCH64_PAGE_SIZE;
            uint8_t* p = (uint8_t*)(uintptr_t)(aarch64_vm_running_high()
                                               ? aarch64_phys_to_virt(pa) : pa);
            for (uint64_t i = 0; i < pages; i++) pmm_set(start + i);
            g_used += pages;
            for (uint64_t i = 0; i < pages * AARCH64_PAGE_SIZE; i++) p[i] = 0;
            return pa;
        }
    }
    return 0;
}

void aarch64_pmm_free(uint64_t pa, uint64_t pages) {
    if (!pa || pa < g_base_pa) return;
    uint64_t start = (pa - g_base_pa) / AARCH64_PAGE_SIZE;
    for (uint64_t i = 0; i < pages && start + i < g_pages; i++) {
        if (pmm_test(start + i)) g_used--;
        pmm_clear(start + i);
    }
}

uint64_t aarch64_pmm_base(void)  { return g_base_pa; }
uint64_t aarch64_pmm_total(void) { return g_pages; }
uint64_t aarch64_pmm_used(void)  { return g_used; }
