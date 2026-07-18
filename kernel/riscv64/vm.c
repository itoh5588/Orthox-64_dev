#include <stdint.h>
#include "pmm.h"
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/vm.h"

#define RISCV64_SATP_MODE_SV39      (8ULL << 60)
#define RISCV64_SATP_PPN_MASK       ((1ULL << 44) - 1ULL)
#define RISCV64_PAGE_SIZE           4096ULL
#define RISCV64_PAGE_MASK           (~(RISCV64_PAGE_SIZE - 1ULL))

#define RISCV64_SV39_PTE_V          (1ULL << 0)
#define RISCV64_SV39_PTE_R          (1ULL << 1)
#define RISCV64_SV39_PTE_W          (1ULL << 2)
#define RISCV64_SV39_PTE_X          (1ULL << 3)
#define RISCV64_SV39_PTE_U          (1ULL << 4)
#define RISCV64_SV39_PTE_G          (1ULL << 5)
#define RISCV64_SV39_PTE_A          (1ULL << 6)
#define RISCV64_SV39_PTE_D          (1ULL << 7)
#define RISCV64_SV39_PTE_FLAG_MASK  0x3ffULL

static uint64_t g_riscv64_kernel_root_pa;
static uint64_t g_riscv64_last_satp;

extern char __kernel_start[];
extern char __kernel_end[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];

static uint64_t riscv64_align_down(uint64_t value) {
    return value & RISCV64_PAGE_MASK;
}

static uint64_t riscv64_align_up(uint64_t value) {
    return (value + RISCV64_PAGE_SIZE - 1ULL) & RISCV64_PAGE_MASK;
}

static void riscv64_memzero(void* ptr, uint64_t size) {
    uint8_t* p = (uint8_t*)ptr;
    while (size--) *p++ = 0;
}

static void riscv64_memcpy(void* dst, const void* src, uint64_t size) {
    uint8_t* out = (uint8_t*)dst;
    const uint8_t* in = (const uint8_t*)src;
    while (size--) *out++ = *in++;
}

static uint64_t riscv64_vm_satp_value(uint64_t root_pa) {
    return RISCV64_SATP_MODE_SV39 | ((root_pa >> 12) & RISCV64_SATP_PPN_MASK);
}

static uint64_t riscv64_vm_flags_to_pte(uint64_t flags) {
    uint64_t pte = RISCV64_SV39_PTE_V | RISCV64_SV39_PTE_A;
    if (flags & RISCV64_VM_PAGE_R) pte |= RISCV64_SV39_PTE_R;
    if (flags & RISCV64_VM_PAGE_W) pte |= RISCV64_SV39_PTE_W | RISCV64_SV39_PTE_D;
    if (flags & RISCV64_VM_PAGE_X) pte |= RISCV64_SV39_PTE_X;
    if (flags & RISCV64_VM_PAGE_U) pte |= RISCV64_SV39_PTE_U;
    if (flags & RISCV64_VM_PAGE_G) pte |= RISCV64_SV39_PTE_G;
    return pte;
}

static uint64_t riscv64_sv39_make_nonleaf(uint64_t table_pa) {
    return ((table_pa >> 12) << 10) | RISCV64_SV39_PTE_V;
}

static uint64_t riscv64_sv39_make_leaf_4k(uint64_t phys_base, uint64_t pte_flags) {
    return ((phys_base >> 12) << 10) | pte_flags;
}

static uint64_t riscv64_sv39_pte_phys(uint64_t entry) {
    return ((entry >> 10) << 12);
}

static int riscv64_sv39_pte_is_leaf(uint64_t entry) {
    return (entry & (RISCV64_SV39_PTE_R | RISCV64_SV39_PTE_W | RISCV64_SV39_PTE_X)) != 0;
}

static uint64_t* riscv64_vm_root_ptr(uint64_t root_pa) {
    return (uint64_t*)(uintptr_t)root_pa;
}

static uint64_t* riscv64_vm_alloc_table(void) {
    void* table_phys = pmm_alloc(1);
    uint64_t* table;
    if (!table_phys) return 0;
    table = (uint64_t*)(uintptr_t)table_phys;
    riscv64_memzero(table, RISCV64_PAGE_SIZE);
    return table;
}

static uint64_t riscv64_sv39_vpn_index(uint64_t virt_addr, int level) {
    return (virt_addr >> (12 + level * 9)) & 0x1ffULL;
}

static uint64_t* riscv64_sv39_walk_leaf(uint64_t* root, uint64_t virt_addr) {
    uint64_t* table = root;
    for (int level = 2; level > 0; level--) {
        uint64_t entry = table[riscv64_sv39_vpn_index(virt_addr, level)];
        if ((entry & RISCV64_SV39_PTE_V) == 0 || riscv64_sv39_pte_is_leaf(entry)) return 0;
        table = (uint64_t*)(uintptr_t)riscv64_sv39_pte_phys(entry);
    }
    return &table[riscv64_sv39_vpn_index(virt_addr, 0)];
}

static uint64_t* riscv64_sv39_walk_create(uint64_t* root, uint64_t virt_addr) {
    uint64_t* table = root;
    uint64_t* kernel_table = riscv64_vm_root_ptr(g_riscv64_kernel_root_pa);
    if (table == kernel_table) kernel_table = 0;

    for (int level = 2; level > 0; level--) {
        uint64_t index = riscv64_sv39_vpn_index(virt_addr, level);
        uint64_t entry = table[index];
        uint64_t kernel_entry = kernel_table ? kernel_table[index] : 0;

        if ((entry & RISCV64_SV39_PTE_V) != 0) {
            uint64_t* child = (uint64_t*)(uintptr_t)riscv64_sv39_pte_phys(entry);
            if (kernel_table && entry == kernel_entry) {
                uint64_t* copy = riscv64_vm_alloc_table();
                if (!copy) return 0;
                riscv64_memcpy(copy, child, RISCV64_PAGE_SIZE);
                table[index] = riscv64_sv39_make_nonleaf((uint64_t)(uintptr_t)copy);
                child = copy;
            } else if (riscv64_sv39_pte_is_leaf(entry)) {
                return 0;
            }
            table = child;
            if (kernel_table && (kernel_entry & RISCV64_SV39_PTE_V) != 0 &&
                !riscv64_sv39_pte_is_leaf(kernel_entry)) {
                kernel_table = (uint64_t*)(uintptr_t)riscv64_sv39_pte_phys(kernel_entry);
            } else {
                kernel_table = 0;
            }
            continue;
        }

        {
            uint64_t* next = riscv64_vm_alloc_table();
            if (!next) return 0;
            table[index] = riscv64_sv39_make_nonleaf((uint64_t)(uintptr_t)next);
            table = next;
        }
        if (kernel_table && (kernel_entry & RISCV64_SV39_PTE_V) != 0 &&
            !riscv64_sv39_pte_is_leaf(kernel_entry)) {
            kernel_table = (uint64_t*)(uintptr_t)riscv64_sv39_pte_phys(kernel_entry);
        } else {
            kernel_table = 0;
        }
    }

    return &table[riscv64_sv39_vpn_index(virt_addr, 0)];
}

static int riscv64_vm_clone_table(uint64_t* dst, const uint64_t* src, const uint64_t* kernel, int level) {
    for (int i = 0; i < 512; i++) {
        uint64_t entry = src[i];
        uint64_t kernel_entry = kernel ? kernel[i] : 0;

        dst[i] = 0;
        if ((entry & RISCV64_SV39_PTE_V) == 0) continue;
        if (kernel && entry == kernel_entry) {
            dst[i] = entry;
            continue;
        }
        if (!riscv64_sv39_pte_is_leaf(entry)) {
            uint64_t* dst_child = riscv64_vm_alloc_table();
            const uint64_t* src_child = (const uint64_t*)(uintptr_t)riscv64_sv39_pte_phys(entry);
            const uint64_t* kernel_child = 0;
            if (!dst_child) return -1;
            dst[i] = riscv64_sv39_make_nonleaf((uint64_t)(uintptr_t)dst_child);
            if (kernel && (kernel_entry & RISCV64_SV39_PTE_V) != 0 &&
                !riscv64_sv39_pte_is_leaf(kernel_entry)) {
                kernel_child = (const uint64_t*)(uintptr_t)riscv64_sv39_pte_phys(kernel_entry);
            }
            if (riscv64_vm_clone_table(dst_child, src_child, kernel_child, level - 1) < 0) return -1;
            continue;
        }

        if (entry & RISCV64_SV39_PTE_U) {
            void* new_page = pmm_alloc(1);
            if (!new_page) return -1;
            riscv64_vm_memcpy_page((uint64_t)(uintptr_t)new_page, riscv64_sv39_pte_phys(entry));
            dst[i] = riscv64_sv39_make_leaf_4k((uint64_t)(uintptr_t)new_page, entry & RISCV64_SV39_PTE_FLAG_MASK);
        } else {
            dst[i] = entry;
        }
    }
    (void)level;
    return 0;
}

static void riscv64_vm_destroy_table(uint64_t* table, const uint64_t* kernel, int free_self) {
    for (int i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        uint64_t kernel_entry = kernel ? kernel[i] : 0;
        if ((entry & RISCV64_SV39_PTE_V) == 0) continue;
        if (kernel && entry == kernel_entry) continue;

        if (!riscv64_sv39_pte_is_leaf(entry)) {
            uint64_t* child = (uint64_t*)(uintptr_t)riscv64_sv39_pte_phys(entry);
            const uint64_t* kernel_child = 0;
            if (kernel && (kernel_entry & RISCV64_SV39_PTE_V) != 0 &&
                !riscv64_sv39_pte_is_leaf(kernel_entry)) {
                kernel_child = (const uint64_t*)(uintptr_t)riscv64_sv39_pte_phys(kernel_entry);
            }
            riscv64_vm_destroy_table(child, kernel_child, 1);
        } else if (entry & RISCV64_SV39_PTE_U) {
            pmm_free((void*)(uintptr_t)riscv64_sv39_pte_phys(entry), 1);
        }
        table[i] = 0;
    }
    if (free_self) {
        pmm_free(table, 1);
    }
}

void riscv64_vm_memcpy_page(uint64_t dst_phys, uint64_t src_phys) {
    riscv64_memcpy((void*)(uintptr_t)dst_phys, (const void*)(uintptr_t)src_phys, RISCV64_PAGE_SIZE);
}

uint64_t riscv64_vm_create_address_space(void) {
    uint64_t* root = riscv64_vm_alloc_table();
    if (!root) return 0;
    riscv64_memcpy(root, riscv64_vm_root_ptr(g_riscv64_kernel_root_pa), RISCV64_PAGE_SIZE);
    return (uint64_t)(uintptr_t)root;
}

void riscv64_vm_destroy_address_space(uint64_t root_pa) {
    uint64_t* root;
    if (!root_pa || root_pa == g_riscv64_kernel_root_pa) return;
    root = riscv64_vm_root_ptr(root_pa);
    riscv64_vm_destroy_table(root, riscv64_vm_root_ptr(g_riscv64_kernel_root_pa), 1);
}

uint64_t riscv64_vm_bootstrap_alloc_page(void) {
    return (uint64_t)(uintptr_t)pmm_alloc(1);
}

void riscv64_vm_bootstrap_free_page(uint64_t phys_addr) {
    pmm_free((void*)(uintptr_t)phys_addr, 1);
}

void riscv64_vm_map_page(uint64_t root_pa, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t* pte = riscv64_sv39_walk_create(riscv64_vm_root_ptr(root_pa), riscv64_align_down(virt_addr));
    if (!pte) {
        riscv64_uart_puts("riscv64 vm: walk create failed\n");
        riscv64_wait_forever();
    }
    *pte = riscv64_sv39_make_leaf_4k(riscv64_align_down(phys_addr), riscv64_vm_flags_to_pte(flags));
}

void riscv64_vm_map_range(uint64_t root_pa, uint64_t virt_addr, uint64_t phys_addr, uint64_t size, uint64_t flags) {
    uint64_t va = riscv64_align_down(virt_addr);
    uint64_t pa = riscv64_align_down(phys_addr);
    uint64_t limit = riscv64_align_up(virt_addr + size);
    while (va < limit) {
        riscv64_vm_map_page(root_pa, va, pa, flags);
        va += RISCV64_PAGE_SIZE;
        pa += RISCV64_PAGE_SIZE;
    }
}

uint64_t riscv64_vm_get_phys(uint64_t root_pa, uint64_t virt_addr) {
    uint64_t* pte = riscv64_sv39_walk_leaf(riscv64_vm_root_ptr(root_pa), virt_addr);
    uint64_t entry;
    if (!pte) return 0;
    entry = *pte;
    if ((entry & RISCV64_SV39_PTE_V) == 0) return 0;
    return riscv64_sv39_pte_phys(entry) | (virt_addr & (RISCV64_PAGE_SIZE - 1ULL));
}

void riscv64_vm_unmap_page(uint64_t root_pa, uint64_t virt_addr) {
    uint64_t* pte = riscv64_sv39_walk_leaf(riscv64_vm_root_ptr(root_pa), virt_addr);
    if (!pte || (*pte & RISCV64_SV39_PTE_V) == 0) return;
    if (*pte & RISCV64_SV39_PTE_U) {
        pmm_free((void*)(uintptr_t)riscv64_sv39_pte_phys(*pte), 1);
    }
    *pte = 0;
    riscv64_sfence_vma();
}

void riscv64_vm_update_page_flags(uint64_t root_pa, uint64_t virt_addr, uint64_t flags) {
    uint64_t* pte = riscv64_sv39_walk_leaf(riscv64_vm_root_ptr(root_pa), virt_addr);
    uint64_t phys;
    if (!pte || (*pte & RISCV64_SV39_PTE_V) == 0) return;
    phys = riscv64_sv39_pte_phys(*pte);
    *pte = riscv64_sv39_make_leaf_4k(phys, riscv64_vm_flags_to_pte(flags));
    riscv64_sfence_vma();
}

uint64_t riscv64_vm_root_pa(void) {
    return g_riscv64_kernel_root_pa;
}

uint64_t riscv64_vm_kernel_address_space(void) {
    return g_riscv64_kernel_root_pa;
}

uint64_t riscv64_vm_current_address_space(void) {
    uint64_t satp = riscv64_read_satp();
    if ((satp >> 60) == 0) return 0;
    return (satp & RISCV64_SATP_PPN_MASK) << 12;
}

void riscv64_vm_activate_address_space(uint64_t root_pa) {
    uint64_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
#ifdef __riscv
    riscv64_uart_puts("  vm activate: 0x");
    riscv64_uart_puthex64(root_pa);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("    text->phys: 0x");
    riscv64_uart_puthex64(riscv64_vm_get_phys(root_pa, (uint64_t)(uintptr_t)__text_start));
    riscv64_uart_puts("\n");
    riscv64_uart_puts("    sp  ->phys: 0x");
    riscv64_uart_puthex64(riscv64_vm_get_phys(root_pa, sp));
    riscv64_uart_puts("\n");
    riscv64_uart_puts("    uart->phys: 0x");
    riscv64_uart_puthex64(riscv64_vm_get_phys(root_pa, RISCV64_QEMU_VIRT_UART0_BASE));
    riscv64_uart_puts("\n");
#endif
    g_riscv64_last_satp = riscv64_vm_satp_value(root_pa);
    riscv64_write_satp(g_riscv64_last_satp);
    riscv64_sfence_vma();
}

uint64_t riscv64_vm_clone_kernel_address_space(void) {
    return riscv64_vm_create_address_space();
}

void riscv64_vm_init(void) {
    const riscv64_boot_info_t* boot = riscv64_boot_info();
    uint64_t* root = riscv64_vm_alloc_table();
    if (!root) {
        riscv64_uart_puts("riscv64 vm: kernel root alloc failed\n");
        riscv64_wait_forever();
    }
    g_riscv64_kernel_root_pa = (uint64_t)(uintptr_t)root;

    if (boot && boot->memory_size != 0) {
        riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                             boot->memory_base,
                             boot->memory_base,
                             boot->memory_size,
                             RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);
    }

    riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                         (uint64_t)(uintptr_t)__text_start,
                         (uint64_t)(uintptr_t)__text_start,
                         (uint64_t)((uintptr_t)__text_end - (uintptr_t)__text_start),
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_X | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                         (uint64_t)(uintptr_t)__rodata_start,
                         (uint64_t)(uintptr_t)__rodata_start,
                         (uint64_t)((uintptr_t)__rodata_end - (uintptr_t)__rodata_start),
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                         (uint64_t)(uintptr_t)__data_start,
                         (uint64_t)(uintptr_t)__data_start,
                         (uint64_t)((uintptr_t)__data_end - (uintptr_t)__data_start),
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                         (uint64_t)(uintptr_t)__bss_start,
                         (uint64_t)(uintptr_t)__bss_start,
                         (uint64_t)((uintptr_t)__bss_end - (uintptr_t)__bss_start),
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);

    riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                         RISCV64_QEMU_VIRT_UART0_BASE,
                         RISCV64_QEMU_VIRT_UART0_BASE,
                         RISCV64_PAGE_SIZE,
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                         RISCV64_QEMU_VIRT_CLINT_BASE,
                         RISCV64_QEMU_VIRT_CLINT_BASE,
                         0x10000ULL,
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);
    riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                         RISCV64_QEMU_VIRT_PLIC_BASE,
                         RISCV64_QEMU_VIRT_PLIC_BASE,
                         0x400000ULL,
                         RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);

    if (boot && boot->dtb_pa && boot->dtb_size) {
        riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                             boot->dtb_pa,
                             boot->dtb_pa,
                             boot->dtb_size,
                             RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_G);
    }

    if (boot && boot->first_virtio_mmio_base) {
        riscv64_vm_map_range(g_riscv64_kernel_root_pa,
                             boot->first_virtio_mmio_base,
                             boot->first_virtio_mmio_base,
                             RISCV64_PAGE_SIZE,
                             RISCV64_VM_PAGE_R | RISCV64_VM_PAGE_W | RISCV64_VM_PAGE_G);
    }

    riscv64_vm_activate_address_space(g_riscv64_kernel_root_pa);

    riscv64_uart_puts("  sv39 satp enabled\n");
    riscv64_uart_puts("  sv39 root: 0x");
    riscv64_uart_puthex64(g_riscv64_kernel_root_pa);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  kernel: 0x");
    riscv64_uart_puthex64((uint64_t)(uintptr_t)__kernel_start);
    riscv64_uart_puts("..0x");
    riscv64_uart_puthex64((uint64_t)(uintptr_t)__kernel_end);
    riscv64_uart_puts("\n");
}

void riscv64_vm_dump_plan(void) {
    riscv64_uart_puts("riscv64 vm\n");
    riscv64_uart_puts("  satp : 0x");
    riscv64_uart_puthex64(riscv64_read_satp());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  root : 0x");
    riscv64_uart_puthex64(g_riscv64_kernel_root_pa);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  text@ : 0x");
    riscv64_uart_puthex64(riscv64_vm_get_phys(g_riscv64_kernel_root_pa, (uint64_t)(uintptr_t)__text_start));
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  stvec: 0x");
    riscv64_uart_puthex64(riscv64_read_stvec());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  sstatus: 0x");
    riscv64_uart_puthex64(riscv64_read_sstatus());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  sie  : 0x");
    riscv64_uart_puthex64(riscv64_read_sie());
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  sip  : 0x");
    riscv64_uart_puthex64(riscv64_read_sip());
    riscv64_uart_puts("\n");
}

arch_address_space_t arch_vm_kernel_address_space(void) {
    return (arch_address_space_t)riscv64_vm_kernel_address_space();
}

arch_address_space_t arch_vm_create_user_address_space(void) {
    return (arch_address_space_t)riscv64_vm_create_address_space();
}

arch_address_space_t arch_vm_clone_address_space(arch_address_space_t address_space) {
    uint64_t* src = riscv64_vm_root_ptr((uint64_t)address_space);
    uint64_t* dst = riscv64_vm_root_ptr(riscv64_vm_create_address_space());
    if (!dst) return 0;
    if (riscv64_vm_clone_table(dst, src, riscv64_vm_root_ptr(g_riscv64_kernel_root_pa), 2) < 0) {
        riscv64_vm_destroy_address_space((uint64_t)(uintptr_t)dst);
        return 0;
    }
    return (arch_address_space_t)(uint64_t)(uintptr_t)dst;
}

void arch_vm_destroy_user_address_space(arch_address_space_t address_space) {
    riscv64_vm_destroy_address_space((uint64_t)address_space);
}

void arch_vm_map_page(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    riscv64_vm_map_page((uint64_t)address_space, vaddr, paddr, flags);
}

void arch_vm_map_range(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    riscv64_vm_map_range((uint64_t)address_space, vaddr, paddr, size, flags);
}

uint64_t arch_vm_get_phys(arch_address_space_t address_space, uint64_t vaddr) {
    return riscv64_vm_get_phys((uint64_t)address_space, vaddr);
}

void arch_vm_unmap_page(arch_address_space_t address_space, uint64_t vaddr) {
    riscv64_vm_unmap_page((uint64_t)address_space, vaddr);
}

void arch_vm_update_page_flags(arch_address_space_t address_space, uint64_t vaddr, uint64_t flags) {
    riscv64_vm_update_page_flags((uint64_t)address_space, vaddr, flags);
}
