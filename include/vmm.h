#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_HUGE     (1ULL << 7)
#define PTE_COW      (1ULL << 9)  // Bit 9: Available for software use
#define PTE_NX       (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_IDX(vaddr) (((vaddr) >> 39) & 0x1FF)
#define PDP_IDX(vaddr)  (((vaddr) >> 30) & 0x1FF)
#define PD_IDX(vaddr)   (((vaddr) >> 21) & 0x1FF)
#define PT_IDX(vaddr)   (((vaddr) >> 12) & 0x1FF)

#define HUGE_PAGE_SIZE (2 * 1024 * 1024)
#define PAGE_SIZE      4096

#define LAPIC_BASE_ADDR 0xFEE00000

// HHDM オフセットを取得・保持するための外部参照
extern uint64_t g_hhdm_offset;

// 物理アドレスを HHDM 仮想アドレスに変換
#define PHYS_TO_VIRT(paddr) ((void*)((uint64_t)(paddr) + g_hhdm_offset))
#define VIRT_TO_PHYS(vaddr) ((uint64_t)(vaddr) - g_hhdm_offset)

void vmm_init(void);
void vmm_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);
uint64_t* vmm_get_kernel_pml4(void);
uint64_t vmm_get_kernel_pml4_phys(void);
void vmm_map_range(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags);
uint64_t vmm_get_phys(uint64_t* pml4, uint64_t vaddr);
void vmm_activate(uint64_t* pml4);
uint64_t vmm_copy_pml4(uint64_t* old_pml4);
void vmm_free_user_pml4(uint64_t pml4_phys);
void vmm_dump_path(uint64_t* pml4, uint64_t vaddr);

struct interrupt_frame;
void vmm_page_fault_handler(struct interrupt_frame* frame);

#endif
