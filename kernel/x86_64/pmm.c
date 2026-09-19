#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "pmm_core.h"
#include "limine.h"

/* 配り方は共通層 (kernel/pmm_core.c)。ここは管理する範囲と管理情報の
 * 置き場を Limine のメモリマップから決めるだけ (include/pmm_core.h)。
 *
 * **以前はここにビットマップ確保があり、毎回ページ 0 から 1 ビットずつ
 * 探していた** (2026-09-19 に共通層の next-fit へ移した) */

// init.c で定義されているリクエストを外部参照
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_hhdm_request hhdm_request;

static uint64_t hhdm_offset;
static void* isa_dma_page = NULL;

/* 管理情報は USABLE 領域から切り出した物理メモリ。HHDM 越しに触る */
void* arch_pmm_meta_ptr(uint64_t pa) {
    return (void*)(uintptr_t)(pa + hhdm_offset);
}

/* 0 埋めは呼ぶ側 (以前の pmm_alloc も埋めていなかった) */
void arch_pmm_after_alloc(uint64_t pa, uint64_t pages) {
    (void)pa;
    (void)pages;
}

void pmm_init(void) {
    struct limine_memmap_response* memmap = memmap_request.response;
    hhdm_offset = hhdm_request.response->offset;

    /* **管理するページ数は RAM の種別だけで決める (2026-09-19)。**
     *
     * 以前は全項目の終わりの最大を取っていた。QEMU の pc は予約領域
     * (種別 RESERVED) を 0xfd00000000 から 12GB 置くので、終わりが 1TiB に
     * なり max_pages = 2^28。-m 2G でもビットマップ 32MB + 参照カウント
     * 512MB = 544MB (RAM の 27%) を最初の USABLE 領域の先頭に取り、
     * pmm_alloc は毎回その 544MB 分のビットを先頭から舐めていた
     * (計測で 1 回平均 141,660 ページ、CoW フォルト 1 回が約 660us)。
     *
     * 予約・フレームバッファ・MMIO は pmm が配らないページなので、範囲に
     * 入れる理由がない。範囲の外のページは pmm_get_ref が 0 を返し、
     * pmm_incref / pmm_free も無視する —— 以前も参照カウント 0 のまま
     * だったので、vm_cow の「pmm が持たないページは共有のまま渡す」は
     * 変わらない */
    uint64_t top_address = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE &&
            entry->type != LIMINE_MEMMAP_ACPI_RECLAIMABLE &&
            entry->type != LIMINE_MEMMAP_ACPI_NVS &&
            entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
            entry->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            continue;
        }
        if (entry->base + entry->length > top_address) {
            top_address = entry->base + entry->length;
        }
    }

    uint64_t max_pages = top_address / PAGE_SIZE;
    /* 参照カウント (uint16_t) の並びを揃えるため、ビットマップの後ろを
     * 8 バイト境界にする */
    uint64_t bitmap_size = ((max_pages + 7) / 8 + 7) & ~7ULL;
    uint64_t total_metadata_size = bitmap_size + max_pages * sizeof(uint16_t);
    uint64_t meta_pa = 0;

    // ビットマップと参照カウントを配置する場所を探す
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= total_metadata_size) {
            meta_pa = entry->base;
            break;
        }
    }
    if (!meta_pa) return;   /* 置き場が無い。pmm は 0 ページのまま */

    pmm_core_setup(0, max_pages, meta_pa, meta_pa + bitmap_size);

    // メモリマップに基づき、利用可能な領域を空きにする
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            pmm_core_mark_free(entry->base, entry->length);
        }
    }

    // メタデータ自身が使用する領域を使用中に戻す
    pmm_core_mark_used(meta_pa, total_metadata_size);

    // Never hand out physical page 0. Many kernel call sites use NULL as
    // the allocation-failure sentinel, so treating page 0 as allocatable
    // would turn a valid allocation into a false failure.
    pmm_core_mark_used(0, PAGE_SIZE);

    // Reserve one ISA-DMA-safe page early before general allocations consume
    // low memory (<16MiB and no 64KiB boundary crossing).
    uint64_t limit_page = (0x01000000ULL / PAGE_SIZE);
    if (limit_page > max_pages) limit_page = max_pages;

    for (uint64_t page = 0; page < limit_page; page++) {
        uint64_t phys = page * PAGE_SIZE;
        if ((phys & 0xFFFFULL) > (0x10000ULL - PAGE_SIZE)) continue;
        if (!pmm_core_reserve_page(phys)) continue;
        isa_dma_page = (void*)phys;
        break;
    }
}

void* pmm_get_isa_dma_page(void) {
    return isa_dma_page;
}
