#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kassert.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "spinlock.h"
#include "limine.h"

extern volatile struct limine_hhdm_request hhdm_request;

uint64_t g_hhdm_offset = 0;
static uint64_t* kernel_pml4;

void puts(const char* s);
void puthex(uint64_t v);

#ifndef ORTHOX_MEM_TRACE
#define ORTHOX_MEM_TRACE 0
#endif

#ifndef ORTHOX_MEM_PROGRESS
#define ORTHOX_MEM_PROGRESS 0
#endif

#if ORTHOX_MEM_PROGRESS
static void vmm_trace_progress_bump(struct task* current, uint64_t* counter) {
    if (!current || !current->trace_progress || !counter) return;
    (*counter)++;
}
#endif

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

static uint64_t rdmsr_local(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void vmm_assert_page_aligned(uint64_t phys) {
    KASSERT((phys & (PAGE_SIZE - 1)) == 0);
}

#if ORTHOX_MEM_TRACE
static int vmm_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}
#endif

#if ORTHOX_MEM_TRACE
static int vmm_memtrace_current_enabled(void) {
    struct task* current = get_current_task();
    return current && vmm_streq(current->comm, "cc1");
}
#endif

static void vmm_memtrace_pf(const char* tag, uint64_t fault_vaddr, uint64_t error_code, uint64_t rip) {
#if ORTHOX_MEM_TRACE
    struct task* current;
    if (!vmm_memtrace_current_enabled()) return;
    current = get_current_task();
    puts("[memtrace] ");
    puts(tag);
    puts(" pid=0x"); puthex(current ? (uint64_t)current->pid : 0);
    puts(" va=0x"); puthex(fault_vaddr);
    puts(" err=0x"); puthex(error_code);
    puts(" rip=0x"); puthex(rip);
    puts(" brk=0x"); puthex(current ? current->heap_break : 0);
    puts(" pmm_alloc=0x"); puthex(pmm_get_allocated_pages());
    puts(" pmm_free=0x"); puthex(pmm_get_free_pages());
    puts("\r\n");
#else
    (void)tag; (void)fault_vaddr; (void)error_code; (void)rip;
#endif
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
    KASSERT(current_table != 0);
    KASSERT(index < 512);
    if (current_table[index] & PTE_PRESENT) {
        vmm_assert_page_aligned(current_table[index] & PTE_ADDR_MASK);
        if (current_table[index] & PTE_HUGE) {
            return (uint64_t*)PHYS_TO_VIRT(current_table[index] & PTE_ADDR_MASK);
        }
        // 既存のテーブルに対しても、要求された権限を追加する
        if (flags & PTE_USER) current_table[index] |= PTE_USER;
        if (flags & PTE_WRITABLE) current_table[index] |= PTE_WRITABLE;
        return (uint64_t*)PHYS_TO_VIRT(current_table[index] & PTE_ADDR_MASK);
    }

    if (!allocate) return NULL;

    void* new_table_phys = pmm_alloc(1);
    if (!new_table_phys) return NULL;

    uint64_t* new_table_virt = (uint64_t*)PHYS_TO_VIRT(new_table_phys);
    for (int i = 0; i < 512; i++) new_table_virt[i] = 0;

    // 新規作成時、要求された権限 (PTE_USER, PTE_WRITABLE) を付与
    uint64_t entry_flags = PTE_PRESENT | (flags & PTE_USER) | (flags & PTE_WRITABLE);
    current_table[index] = (uint64_t)new_table_phys | entry_flags;

    return new_table_virt;
}

void vmm_init(void) {
    g_hhdm_offset = hhdm_request.response->offset;

    void* pml4_phys = pmm_alloc(1);
    kernel_pml4 = (uint64_t*)PHYS_TO_VIRT(pml4_phys);
    for (int i = 0; i < 512; i++) kernel_pml4[i] = 0;

    vmm_map_range(kernel_pml4, 0, 0, 0x100000000ULL, PTE_PRESENT | PTE_WRITABLE);
    
    extern volatile struct limine_executable_address_request kernel_address_request;
    if (kernel_address_request.response) {
        struct limine_executable_address_response* kaddr = kernel_address_request.response;
        vmm_map_range(kernel_pml4, kaddr->virtual_base, kaddr->physical_base, 0x2000000, PTE_PRESENT | PTE_WRITABLE);
    }
}

void vmm_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    KASSERT(pml4 != 0);
    vmm_assert_page_aligned(paddr);
    uint64_t* pdp = get_next_level(pml4, PML4_IDX(vaddr), true, flags);
    uint64_t* pd  = get_next_level(pdp, PDP_IDX(vaddr), true, flags);
    uint64_t* pt  = get_next_level(pd, PD_IDX(vaddr), true, flags);

    pt[PT_IDX(vaddr)] = (paddr & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

void vmm_map_huge_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    KASSERT(pml4 != 0);
    KASSERT((vaddr & (HUGE_PAGE_SIZE - 1)) == 0);
    KASSERT((paddr & (HUGE_PAGE_SIZE - 1)) == 0);
    uint64_t* pdp = get_next_level(pml4, PML4_IDX(vaddr), true, flags);
    uint64_t* pd  = get_next_level(pdp, PDP_IDX(vaddr), true, flags);

    pd[PD_IDX(vaddr)] = (paddr & PTE_ADDR_MASK) | flags | PTE_PRESENT | PTE_HUGE;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

void vmm_map_range(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    uint64_t curr = 0;
    while (curr < size) {
        if ((size - curr) >= HUGE_PAGE_SIZE && 
            (vaddr + curr) % HUGE_PAGE_SIZE == 0 && 
            (paddr + curr) % HUGE_PAGE_SIZE == 0) {
            vmm_map_huge_page(pml4, vaddr + curr, paddr + curr, flags);
            curr += HUGE_PAGE_SIZE;
        } else {
            vmm_map_page(pml4, vaddr + curr, paddr + curr, flags);
            curr += PAGE_SIZE;
        }
    }
}

uint64_t vmm_get_phys(uint64_t* pml4, uint64_t vaddr) {
    KASSERT(pml4 != 0);
    if (!(pml4[PML4_IDX(vaddr)] & PTE_PRESENT)) return 0;
    uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT(pml4[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
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

void vmm_activate(uint64_t* pml4) {
    uint64_t pml4_phys = VIRT_TO_PHYS(pml4);
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t* vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}

uint64_t vmm_get_kernel_pml4_phys(void) {
    return VIRT_TO_PHYS(kernel_pml4);
}

uint64_t vmm_copy_pml4(uint64_t* old_pml4) {
    KASSERT(old_pml4 != 0);
    void* new_pml4_phys = pmm_alloc(1);
    if (!new_pml4_phys) return 0;
    uint64_t* new_pml4 = (uint64_t*)PHYS_TO_VIRT(new_pml4_phys);

    for (int i = 256; i < 512; i++) {
        new_pml4[i] = old_pml4[i];
    }

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

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");

    return (uint64_t)new_pml4_phys;
}

void vmm_free_user_pml4(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    KASSERT(pml4_phys != vmm_get_kernel_pml4_phys());
    vmm_assert_page_aligned(pml4_phys);

    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT((void*)pml4_phys);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;

        uint64_t pdp_phys = pml4[i] & PTE_ADDR_MASK;
        uint64_t* pdp = (uint64_t*)PHYS_TO_VIRT((void*)pdp_phys);
        for (int j = 0; j < 512; j++) {
            if (!(pdp[j] & PTE_PRESENT)) continue;

            uint64_t pd_phys = pdp[j] & PTE_ADDR_MASK;
            vmm_assert_page_aligned(pd_phys);
            uint64_t* pd = (uint64_t*)PHYS_TO_VIRT((void*)pd_phys);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;

                if (pd[k] & PTE_HUGE) {
                    KASSERT(pmm_get_ref((void*)(pd[k] & PTE_ADDR_MASK)) > 0);
                    pmm_free((void*)(pd[k] & PTE_ADDR_MASK), 512);
                    continue;
                }

                uint64_t pt_phys = pd[k] & PTE_ADDR_MASK;
                vmm_assert_page_aligned(pt_phys);
                uint64_t* pt = (uint64_t*)PHYS_TO_VIRT((void*)pt_phys);
                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PTE_PRESENT)) continue;
                    KASSERT(pmm_get_ref((void*)(pt[l] & PTE_ADDR_MASK)) > 0);
                    pmm_free((void*)(pt[l] & PTE_ADDR_MASK), 1);
                }
                pmm_free((void*)pt_phys, 1);
            }
            pmm_free((void*)pd_phys, 1);
        }
        pmm_free((void*)pdp_phys, 1);
    }
    pmm_free((void*)pml4_phys, 1);
}

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t int_no, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

static void kill_current_task_on_user_fault(struct interrupt_frame* frame, uint64_t fault_vaddr, const char* reason) {
    if (frame->error_code & 4) {
        struct task* current = get_current_task();
        if (current) {
            uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(current->ctx.cr3);
            puts("#PF(User): ");
            puts(reason);
            puts(" at 0x"); puthex(fault_vaddr);
            puts(" Error: "); puthex(frame->error_code);
            puts(" PID: "); puthex((uint64_t)current->pid);
            puts(" RIP: "); puthex(frame->rip);
            puts(" RSP: "); puthex(frame->rsp);
            dump_fault_cpu_state();
            puts("\r\n");
            if (vmm_get_phys(pml4, frame->rsp) != 0) {
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

static const char* classify_user_fault_reason(struct interrupt_frame* frame, uint64_t fault_vaddr) {
    struct task* current = get_current_task();
    if (current && current->user_stack_guard && current->user_stack_bottom) {
        if (fault_vaddr >= current->user_stack_guard && fault_vaddr < current->user_stack_bottom) {
            return (frame->error_code & 2) ? "stack-guard-write" : "stack-overflow";
        }
    }
    return 0;
}

void vmm_page_fault_handler(struct interrupt_frame* frame) {
    uint64_t fault_vaddr;
    struct task* current = get_current_task();
    (void)current;
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
        for(;;) __asm__("hlt");
    }

    if (!(frame->error_code & 2)) {
        kill_current_task_on_user_fault(frame, fault_vaddr, "read/page-not-present");
        puts("#PF: Read access violation at 0x"); puthex(fault_vaddr);
        puts(" RIP: "); puthex(frame->rip);
        puts(" RSP: "); puthex(frame->rsp);
        puts(" ERR: "); puthex(frame->error_code);
        dump_fault_cpu_state();
        puts("\r\n");
        for(;;) __asm__("hlt");
    }

    uint64_t* cr3_virt;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_virt));
    uint64_t* pml4 = (uint64_t*)PHYS_TO_VIRT(cr3_virt);

    uint64_t pml4e = pml4[PML4_IDX(fault_vaddr)];
    if (!(pml4e & PTE_PRESENT)) {
        kill_current_task_on_user_fault(frame, fault_vaddr, "pml4e-not-present");
        puts("#PF: Unexpected page fault at 0x"); puthex(fault_vaddr);
        puts(" RIP: "); puthex(frame->rip);
        puts(" RSP: "); puthex(frame->rsp);
        puts(" ERR: "); puthex(frame->error_code);
        dump_fault_cpu_state();
        puts("\r\n");
        for(;;) __asm__("hlt");
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
        for(;;) __asm__("hlt");
    }
    uint64_t* pd  = (uint64_t*)PHYS_TO_VIRT(pdpe & PTE_ADDR_MASK);
    
    if (pd[PD_IDX(fault_vaddr)] & PTE_HUGE) {
        if (pd[PD_IDX(fault_vaddr)] & PTE_COW) {
            vmm_memtrace_pf("pf-cow-huge", fault_vaddr, frame->error_code, frame->rip);
            void* old_page_phys = (void*)(pd[PD_IDX(fault_vaddr)] & PTE_ADDR_MASK);
            KASSERT(pmm_get_ref(old_page_phys) > 0);
            if (pmm_get_ref(old_page_phys) > 1) {
                void* new_page_phys = pmm_alloc(512); // 2MB
                for (uint64_t i = 0; i < HUGE_PAGE_SIZE; i++) {
                    ((uint8_t*)PHYS_TO_VIRT(new_page_phys))[i] = ((uint8_t*)PHYS_TO_VIRT(old_page_phys))[i];
                }
                pd[PD_IDX(fault_vaddr)] = (uint64_t)new_page_phys | (pd[PD_IDX(fault_vaddr)] & ~PTE_ADDR_MASK & ~PTE_COW) | PTE_WRITABLE;
                pmm_free(old_page_phys, 512);
            } else {
                pd[PD_IDX(fault_vaddr)] = (pd[PD_IDX(fault_vaddr)] & ~PTE_COW) | PTE_WRITABLE;
            }
            __asm__ volatile("invlpg (%0)" : : "r"(fault_vaddr) : "memory");
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
            for(;;) __asm__("hlt");
        }
        uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(fault_vaddr)] & PTE_ADDR_MASK);
        uint64_t* pte = &pt[PT_IDX(fault_vaddr)];

        if (*pte & PTE_COW) {
#if ORTHOX_MEM_PROGRESS
            vmm_trace_progress_bump(current, &current->trace_cow_faults);
#endif
            vmm_memtrace_pf("pf-cow", fault_vaddr, frame->error_code, frame->rip);
            void* old_page_phys = (void*)(*pte & PTE_ADDR_MASK);
            KASSERT(pmm_get_ref(old_page_phys) > 0);
            if (pmm_get_ref(old_page_phys) > 1) {
                void* new_page_phys = pmm_alloc(1);
                if (!new_page_phys) {
                    kill_current_task_on_user_fault(frame, fault_vaddr, "cow-alloc-failed");
                    for(;;) __asm__("hlt");
                }
                for (int i = 0; i < 4096; i++) {
                    ((uint8_t*)PHYS_TO_VIRT(new_page_phys))[i] = ((uint8_t*)PHYS_TO_VIRT(old_page_phys))[i];
                }
                *pte = (uint64_t)new_page_phys | (*pte & ~PTE_ADDR_MASK & ~PTE_COW) | PTE_WRITABLE;
                pmm_free(old_page_phys, 1);
            } else {
                *pte = (*pte & ~PTE_COW) | PTE_WRITABLE;
            }
            __asm__ volatile("invlpg (%0)" : : "r"(fault_vaddr) : "memory");
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
    for(;;) __asm__("hlt");
}
