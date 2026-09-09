#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

// 物理メモリマネージャーの初期化
void pmm_init(void);

// 指定したページ数分、連続した物理メモリを確保して物理アドレスを返す
void* pmm_alloc(size_t pages);

// 指定した物理アドレスからページ数分を解放する
void pmm_free(void* addr, size_t pages);

// 参照カウントを増やす
void pmm_incref(void* addr);

// 参照カウントを取得する
uint16_t pmm_get_ref(void* addr);

// 16MiB 未満 + 64KiB 境界跨ぎ無しの ISA DMA 用 4KiB ページを返す
// 取得済みページを返すのみで追加確保は行わない
void* pmm_get_isa_dma_page(void);

uint64_t pmm_get_allocated_pages(void);
uint64_t pmm_get_free_pages(void);
uint64_t pmm_get_total_pages(void);

#endif
