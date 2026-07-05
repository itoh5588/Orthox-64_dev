#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pmm.h"
#include "limine.h"
#include "spinlock.h"

// init.c で定義されているリクエストを外部参照
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_hhdm_request hhdm_request;

static uint8_t* bitmap;
static uint16_t* ref_counts;
static uint64_t max_pages;
static uint64_t allocated_pages;
static uint64_t hhdm_offset;
static void* isa_dma_page = NULL;
static spinlock_t g_pmm_lock;

// ビットマップ操作用の補助関数
static inline void bitmap_set(uint64_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(uint64_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline bool bitmap_test(uint64_t page) {
    return (bitmap[page / 8] >> (page % 8)) & 1;
}

void pmm_init(void) {
    struct limine_memmap_response* memmap = memmap_request.response;
    hhdm_offset = hhdm_request.response->offset;
    spinlock_init(&g_pmm_lock);

    uint64_t top_address = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->base + entry->length > top_address) {
            top_address = entry->base + entry->length;
        }
    }

    max_pages = top_address / PAGE_SIZE;
    uint64_t bitmap_size = (max_pages + 7) / 8;
    uint64_t ref_counts_size = max_pages * sizeof(uint16_t);
    uint64_t total_metadata_size = bitmap_size + ref_counts_size;

    // ビットマップと参照カウントを配置する場所を探す
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= total_metadata_size) {
            bitmap = (uint8_t*)(entry->base + hhdm_offset);
            ref_counts = (uint16_t*)((uint64_t)bitmap + bitmap_size);

            // メタデータ領域を初期化
            for (uint64_t j = 0; j < bitmap_size; j++) bitmap[j] = 0xFF;
            for (uint64_t j = 0; j < max_pages; j++) ref_counts[j] = 0;
            
            // メタデータ自身が使用する領域をスキップ
            uint64_t pages_needed = (total_metadata_size + PAGE_SIZE - 1) / PAGE_SIZE;
            entry->base += pages_needed * PAGE_SIZE;
            entry->length -= pages_needed * PAGE_SIZE;
            break;
        }
    }

    // メモリマップに基づき、利用可能な領域をビットマップでマーク
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE) {
                bitmap_clear((entry->base + j) / PAGE_SIZE);
            }
        }
    }

    // Reserve one ISA-DMA-safe page early before general allocations consume
    // low memory (<16MiB and no 64KiB boundary crossing).
    uint64_t limit_page = (0x01000000ULL / PAGE_SIZE);
    if (limit_page > max_pages) limit_page = max_pages;

    // Never hand out physical page 0. Many kernel call sites use NULL as
    // the allocation-failure sentinel, so treating page 0 as allocatable
    // would turn a valid allocation into a false failure.
    if (max_pages > 0) {
        bitmap_set(0);
        ref_counts[0] = 1;
    }

    for (uint64_t page = 0; page < limit_page; page++) {
        if (bitmap_test(page)) continue;
        uint64_t phys = page * PAGE_SIZE;
        if ((phys & 0xFFFFULL) > (0x10000ULL - PAGE_SIZE)) continue;
        bitmap_set(page);
        ref_counts[page] = 1;
        isa_dma_page = (void*)phys;
        break;
    }
}

void* pmm_alloc(size_t pages) {
    uint64_t consecutive_pages = 0;
    uint64_t start_page = 0;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    for (uint64_t i = 0; i < max_pages; i++) {
        if (!bitmap_test(i)) {
            if (consecutive_pages == 0) start_page = i;
            consecutive_pages++;
            if (consecutive_pages == pages) {
                for (uint64_t j = 0; j < pages; j++) {
                    bitmap_set(start_page + j);
                    ref_counts[start_page + j] = 1;
                }
                allocated_pages += pages;
                spin_unlock_irqrestore(&g_pmm_lock, flags);
                return (void*)(start_page * PAGE_SIZE);
            }
        } else {
            consecutive_pages = 0;
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return NULL; // メモリ不足
}

void pmm_free(void* addr, size_t pages) {
    uint64_t start_page = (uint64_t)addr / PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t page = start_page + i;
        if (page < max_pages && ref_counts[page] > 0) {
            ref_counts[page]--;
            if (ref_counts[page] == 0) {
                bitmap_clear(page);
                if (allocated_pages > 0) allocated_pages--;
            }
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_incref(void* addr) {
    uint64_t page = (uint64_t)addr / PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    if (page < max_pages) {
        ref_counts[page]++;
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

uint16_t pmm_get_ref(void* addr) {
    uint64_t page = (uint64_t)addr / PAGE_SIZE;
    uint16_t ref = 0;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    if (page < max_pages) {
        ref = ref_counts[page];
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return ref;
}

void* pmm_get_isa_dma_page(void) {
    return isa_dma_page;
}

uint64_t pmm_get_allocated_pages(void) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    uint64_t pages = allocated_pages;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return pages;
}

uint64_t pmm_get_free_pages(void) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    uint64_t free_pages = 0;
    for (uint64_t i = 0; i < max_pages; i++) {
        if (!bitmap_test(i)) free_pages++;
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return free_pages;
}

uint64_t pmm_get_total_pages(void) {
    return max_pages;
}
