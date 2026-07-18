#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "x86_64/mmu.h"
#include "x86_64/vm_backend.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"

void puts(const char* s);
void puthex(uint64_t v);

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

static uint64_t rdmsr_local(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void dump_fault_cpu_state(void) {
    uint64_t gs_base = rdmsr_local(MSR_GS_BASE);
    uint64_t kernel_gs_base = rdmsr_local(MSR_KERNEL_GS_BASE);
    struct cpu_local* cpu = get_cpu_local();
    struct task* current = get_current_task();

    puts(" GS_BASE: "); puthex(gs_base);
    puts(" KGS_BASE: "); puthex(kernel_gs_base);
    puts(" CPU: "); puthex(cpu ? (uint64_t)cpu->cpu_id : 0xFFFFFFFFFFFFFFFFULL);
    puts(" CUR: "); puthex((uint64_t)(uintptr_t)current);
    puts("\r\n");
}

static uint64_t* get_next_level(uint64_t* current_table, uint64_t index, bool allocate, uint64_t flags) {
    if (current_table[index] & PTE_PRESENT) {
        if (current_table[index] & PTE_HUGE) {
            return (uint64_t*)PHYS_TO_VIRT(current_table[index] & PTE_ADDR_MASK);
        }
        if (flags & PTE_USER) current_table[index] |= PTE_USER;
        if (flags & PTE_WRITABLE) current_table[index] |= PTE_WRITABLE;
        return (uint64_t*)PHYS_TO_VIRT(current_table[index] & PTE_ADDR_MASK);
    }

    if (!allocate) return NULL;

    void* new_table_phys = pmm_alloc(1);
    if (!new_table_phys) return NULL;

    uint64_t* new_table_virt = (uint64_t*)PHYS_TO_VIRT(new_table_phys);
    for (int i = 0; i < 512; i++) new_table_virt[i] = 0;

    current_table[index] = (uint64_t)new_table_phys | PTE_PRESENT | (flags & PTE_USER) | (flags & PTE_WRITABLE);
    return new_table_virt;
}

void arch_vm_backend_init(uint64_t* kernel_root) {
    (void)kernel_root;
}

void arch_vm_backend_map_page(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t* pdp = get_next_level(root, PML4_IDX(vaddr), true, flags);
    uint64_t* pd  = get_next_level(pdp, PDP_IDX(vaddr), true, flags);
    uint64_t* pt  = get_next_level(pd, PD_IDX(vaddr), true, flags);

    pt[PT_IDX(vaddr)] = (paddr & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    arch_mmu_invalidate_page(vaddr);
}

static void arch_vm_backend_map_huge_page(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t* pdp = get_next_level(root, PML4_IDX(vaddr), true, flags);
    uint64_t* pd  = get_next_level(pdp, PDP_IDX(vaddr), true, flags);

    pd[PD_IDX(vaddr)] = (paddr & PTE_ADDR_MASK) | flags | PTE_PRESENT | PTE_HUGE;
    arch_mmu_invalidate_page(vaddr);
}

void arch_vm_backend_map_range(uint64_t* root, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    uint64_t curr = 0;
    while (curr < size) {
        if ((size - curr) >= HUGE_PAGE_SIZE &&
            (vaddr + curr) % HUGE_PAGE_SIZE == 0 &&
            (paddr + curr) % HUGE_PAGE_SIZE == 0) {
            arch_vm_backend_map_huge_page(root, vaddr + curr, paddr + curr, flags);
            curr += HUGE_PAGE_SIZE;
        } else {
            arch_vm_backend_map_page(root, vaddr + curr, paddr + curr, flags);
            curr += PAGE_SIZE;
        }
    }
}

uint64_t arch_vm_backend_get_phys(uint64_t* root, uint64_t vaddr) {
    if (!(root[PML4_IDX(vaddr)] & PTE_PRESENT)) return 0;
    uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT(root[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pdp[PDP_IDX(vaddr)] & PTE_PRESENT)) return 0;
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    if (pd[PD_IDX(vaddr)] & PTE_HUGE) {
        return (pd[PD_IDX(vaddr)] & PTE_ADDR_MASK) + (vaddr % HUGE_PAGE_SIZE);
    }
    if (!(pd[PD_IDX(vaddr)] & PTE_PRESENT)) return 0;
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pt[PT_IDX(vaddr)] & PTE_PRESENT)) return 0;
    return (pt[PT_IDX(vaddr)] & PTE_ADDR_MASK) + (vaddr % PAGE_SIZE);
}

arch_address_space_t arch_vm_backend_clone_address_space(uint64_t* old_pml4) {
    void* new_pml4_phys = pmm_alloc(1);
    uint64_t* new_pml4 = (uint64_t*)PHYS_TO_VIRT(new_pml4_phys);

    for (int i = 256; i < 512; i++) new_pml4[i] = old_pml4[i];

    for (int i = 0; i < 256; i++) {
        if (!(old_pml4[i] & PTE_PRESENT)) {
            new_pml4[i] = 0;
            continue;
        }

        void* new_pdp_phys = pmm_alloc(1);
        new_pml4[i] = (uint64_t)new_pdp_phys | (old_pml4[i] & ~PTE_ADDR_MASK);
        uint64_t* old_pdp = (uint64_t*)PHYS_TO_VIRT(old_pml4[i] & PTE_ADDR_MASK);
        uint64_t* new_pdp = (uint64_t*)PHYS_TO_VIRT(new_pdp_phys);

        for (int j = 0; j < 512; j++) {
            if (!(old_pdp[j] & PTE_PRESENT)) {
                new_pdp[j] = 0;
                continue;
            }

            void* new_pd_phys = pmm_alloc(1);
            new_pdp[j] = (uint64_t)new_pd_phys | (old_pdp[j] & ~PTE_ADDR_MASK);
            uint64_t* old_pd = (uint64_t*)PHYS_TO_VIRT(old_pdp[j] & PTE_ADDR_MASK);
            uint64_t* new_pd = (uint64_t*)PHYS_TO_VIRT(new_pd_phys);

            for (int k = 0; k < 512; k++) {
                if (!(old_pd[k] & PTE_PRESENT)) {
                    new_pd[k] = 0;
                    continue;
                }

                if (old_pd[k] & PTE_HUGE) {
                    if (old_pd[k] & PTE_WRITABLE) {
                        old_pd[k] = (old_pd[k] & ~PTE_WRITABLE) | PTE_COW;
                    }
                    new_pd[k] = old_pd[k];
                    pmm_incref((void*)(old_pd[k] & PTE_ADDR_MASK));
                    continue;
                }

                void* new_pt_phys = pmm_alloc(1);
                new_pd[k] = (uint64_t)new_pt_phys | (old_pd[k] & ~PTE_ADDR_MASK);
                uint64_t* old_pt = (uint64_t*)PHYS_TO_VIRT(old_pd[k] & PTE_ADDR_MASK);
                uint64_t* new_pt = (uint64_t*)PHYS_TO_VIRT(new_pt_phys);

                for (int l = 0; l < 512; l++) {
                    if (!(old_pt[l] & PTE_PRESENT)) {
                        new_pt[l] = 0;
                        continue;
                    }
                    if (old_pt[l] & PTE_WRITABLE) {
                        old_pt[l] = (old_pt[l] & ~PTE_WRITABLE) | PTE_COW;
                    }
                    new_pt[l] = old_pt[l];
                    pmm_incref((void*)(old_pt[l] & PTE_ADDR_MASK));
                }
            }
        }
    }

    arch_mmu_reload_address_space();
    return (arch_address_space_t)(uint64_t)new_pml4_phys;
}

void arch_vm_backend_destroy_user_address_space(arch_address_space_t address_space) {
    if (!address_space) return;

    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT((void*)address_space);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;
        uint64_t pdp_phys = pml4[i] & PTE_ADDR_MASK;
        uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT((void*)pdp_phys);
        for (int j = 0; j < 512; j++) {
            if (!(pdp[j] & PTE_PRESENT)) continue;
            uint64_t pd_phys = pdp[j] & PTE_ADDR_MASK;
            uint64_t* pd = (uint64_t*)PHYS_TO_VIRT((void*)pd_phys);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                if (pd[k] & PTE_HUGE) {
                    pmm_free((void*)(pd[k] & PTE_ADDR_MASK), 512);
                    continue;
                }
                uint64_t pt_phys = pd[k] & PTE_ADDR_MASK;
                uint64_t* pt = (uint64_t*)PHYS_TO_VIRT((void*)pt_phys);
                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PTE_PRESENT)) continue;
                    pmm_free((void*)(pt[l] & PTE_ADDR_MASK), 1);
                }
                pmm_free((void*)pt_phys, 1);
            }
            pmm_free((void*)pd_phys, 1);
        }
        pmm_free((void*)pdp_phys, 1);
    }
    pmm_free((void*)address_space, 1);
}

void arch_vm_backend_unmap_page(uint64_t* root, uint64_t vaddr) {
    if (!(root[PML4_IDX(vaddr)] & PTE_PRESENT)) return;
    uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT(root[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pdp[PDP_IDX(vaddr)] & PTE_PRESENT)) return;
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pd[PD_IDX(vaddr)] & PTE_PRESENT)) return;
    if (pd[PD_IDX(vaddr)] & PTE_HUGE) return;
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    uint64_t* pte = &pt[PT_IDX(vaddr)];
    if (!(*pte & PTE_PRESENT)) return;
    void* page_phys = (void*)(*pte & PTE_ADDR_MASK);
    *pte = 0;
    arch_mmu_invalidate_page(vaddr);
    pmm_free(page_phys, 1);
}

void arch_vm_backend_update_page_flags(uint64_t* root, uint64_t vaddr, uint64_t new_flags) {
    uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT(root[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    uint64_t* pte = &pt[PT_IDX(vaddr)];
    uint64_t merged = *pte | (new_flags & PTE_WRITABLE);
    if (!(new_flags & PTE_NX)) merged &= ~PTE_NX;
    *pte = merged;
}

static void kill_current_task_on_user_fault(arch_interrupt_frame_t* frame, uint64_t fault_vaddr, const char* reason) {
    if (frame->error_code & 4) {
        struct task* current = get_current_task();
        if (current) {
            arch_address_space_t address_space = arch_task_context_get_address_space(&current->ctx);
            puts("#PF(User): ");
            puts(reason);
            puts(" at 0x"); puthex(fault_vaddr);
            puts(" Error: "); puthex(frame->error_code);
            puts(" PID: "); puthex((uint64_t)current->pid);
            puts(" RIP: "); puthex(frame->rip);
            puts(" RSP: "); puthex(frame->rsp);
            dump_fault_cpu_state();
            puts("\r\n");
            if (arch_vm_get_phys(address_space, frame->rsp) != 0) {
                uint64_t* user_rsp = (uint64_t*)frame->rsp;
                puts("  [RSP+0x00] = "); puthex(user_rsp[0]); puts("\r\n");
                puts("  [RSP+0x08] = "); puthex(user_rsp[1]); puts("\r\n");
                puts("  [RSP+0x10] = "); puthex(user_rsp[2]); puts("\r\n");
                puts("  [RSP+0x18] = "); puthex(user_rsp[3]); puts("\r\n");
            }
            task_mark_zombie(current, 139);
            while (1) kernel_yield();
        }
    }
}

static const char* classify_user_fault_reason(arch_interrupt_frame_t* frame, uint64_t fault_vaddr) {
    struct task* current = get_current_task();
    if (current && current->user_stack_guard && current->user_stack_bottom) {
        if (fault_vaddr >= current->user_stack_guard && fault_vaddr < current->user_stack_bottom) {
            return (frame->error_code & 2) ? "stack-guard-write" : "stack-overflow";
        }
    }
    return 0;
}

void arch_vm_backend_handle_page_fault(arch_interrupt_frame_t* frame) {
    uint64_t fault_vaddr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_vaddr));

    const char* stack_reason = classify_user_fault_reason(frame, fault_vaddr);
    if (stack_reason) {
        kill_current_task_on_user_fault(frame, fault_vaddr, stack_reason);
        puts("#PF: User stack guard hit at 0x"); puthex(fault_vaddr);
        puts(" RIP: "); puthex(frame->rip);
        puts(" RSP: "); puthex(frame->rsp);
        puts(" ERR: "); puthex(frame->error_code);
        dump_fault_cpu_state();
        puts("\r\n");
        for (;;) __asm__("hlt");
    }

    if (!(frame->error_code & 2)) {
        kill_current_task_on_user_fault(frame, fault_vaddr, "read/page-not-present");
        puts("#PF: Read access violation at 0x"); puthex(fault_vaddr);
        puts(" RIP: "); puthex(frame->rip);
        puts(" RSP: "); puthex(frame->rsp);
        puts(" ERR: "); puthex(frame->error_code);
        dump_fault_cpu_state();
        puts("\r\n");
        for (;;) __asm__("hlt");
    }

    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(arch_mmu_read_address_space());
    uint64_t pml4e = pml4[PML4_IDX(fault_vaddr)];
    if (!(pml4e & PTE_PRESENT)) {
        kill_current_task_on_user_fault(frame, fault_vaddr, "pml4e-not-present");
        puts("#PF: Unexpected page fault at 0x"); puthex(fault_vaddr);
        puts(" RIP: "); puthex(frame->rip);
        puts(" RSP: "); puthex(frame->rsp);
        puts(" ERR: "); puthex(frame->error_code);
        dump_fault_cpu_state();
        puts("\r\n");
        for (;;) __asm__("hlt");
    }
    uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT(pml4e & PTE_ADDR_MASK);
    uint64_t pdpe = pdp[PDP_IDX(fault_vaddr)];
    if (!(pdpe & PTE_PRESENT)) {
        kill_current_task_on_user_fault(frame, fault_vaddr, "pdpe-not-present");
        puts("#PF: Unexpected page fault at 0x"); puthex(fault_vaddr);
        puts(" RIP: "); puthex(frame->rip);
        puts(" RSP: "); puthex(frame->rsp);
        puts(" ERR: "); puthex(frame->error_code);
        dump_fault_cpu_state();
        puts("\r\n");
        for (;;) __asm__("hlt");
    }
    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdpe & PTE_ADDR_MASK);

    if (pd[PD_IDX(fault_vaddr)] & PTE_HUGE) {
        if (pd[PD_IDX(fault_vaddr)] & PTE_COW) {
            void* old_page_phys = (void*)(pd[PD_IDX(fault_vaddr)] & PTE_ADDR_MASK);
            if (pmm_get_ref(old_page_phys) > 1) {
                void* new_page_phys = pmm_alloc(512);
                for (uint64_t i = 0; i < HUGE_PAGE_SIZE; i++) {
                    ((uint8_t*)PHYS_TO_VIRT(new_page_phys))[i] = ((uint8_t*)PHYS_TO_VIRT(old_page_phys))[i];
                }
                pd[PD_IDX(fault_vaddr)] = (uint64_t)new_page_phys | (pd[PD_IDX(fault_vaddr)] & ~PTE_ADDR_MASK & ~PTE_COW) | PTE_WRITABLE;
                pmm_free(old_page_phys, 512);
            } else {
                pd[PD_IDX(fault_vaddr)] = (pd[PD_IDX(fault_vaddr)] & ~PTE_COW) | PTE_WRITABLE;
            }
            arch_mmu_invalidate_page(fault_vaddr);
            return;
        }
    } else {
        if (!(pd[PD_IDX(fault_vaddr)] & PTE_PRESENT)) {
            kill_current_task_on_user_fault(frame, fault_vaddr, "pde-not-present");
            puts("#PF: Unexpected page fault at 0x"); puthex(fault_vaddr);
            puts(" RIP: "); puthex(frame->rip);
            puts(" RSP: "); puthex(frame->rsp);
            puts(" ERR: "); puthex(frame->error_code);
            dump_fault_cpu_state();
            puts("\r\n");
            for (;;) __asm__("hlt");
        }
        uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(fault_vaddr)] & PTE_ADDR_MASK);
        uint64_t* pte = &pt[PT_IDX(fault_vaddr)];
        if (*pte & PTE_COW) {
            void* old_page_phys = (void*)(*pte & PTE_ADDR_MASK);
            if (pmm_get_ref(old_page_phys) > 1) {
                void* new_page_phys = pmm_alloc(1);
                for (int i = 0; i < 4096; i++) {
                    ((uint8_t*)PHYS_TO_VIRT(new_page_phys))[i] = ((uint8_t*)PHYS_TO_VIRT(old_page_phys))[i];
                }
                *pte = (uint64_t)new_page_phys | (*pte & ~PTE_ADDR_MASK & ~PTE_COW) | PTE_WRITABLE;
                pmm_free(old_page_phys, 1);
            } else {
                *pte = (*pte & ~PTE_COW) | PTE_WRITABLE;
            }
            arch_mmu_invalidate_page(fault_vaddr);
            return;
        }
    }

    kill_current_task_on_user_fault(frame, fault_vaddr, "write-to-nonwritable");
    puts("#PF: Unexpected page fault at 0x"); puthex(fault_vaddr);
    puts(" RIP: "); puthex(frame->rip);
    puts(" RSP: "); puthex(frame->rsp);
    puts(" ERR: "); puthex(frame->error_code);
    dump_fault_cpu_state();
    puts("\r\n");
    for (;;) __asm__("hlt");
}
