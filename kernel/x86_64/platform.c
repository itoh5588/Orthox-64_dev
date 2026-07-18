#include <stdint.h>
#include "x86_64/platform.h"
#include "x86_64/trap.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"

extern void pic_init(void);

static void arch_x86_64_enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |= (1ULL << 1);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);
    cr4 |= (1ULL << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

static void arch_x86_64_enable_paging_features(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void arch_platform_init_bsp(void) {
    gdt_init();
    arch_x86_64_enable_sse();
    arch_x86_64_enable_paging_features();
    arch_trap_init_bsp();
    pic_init();
    lapic_init();
}

void arch_platform_init_ap(uint32_t cpu_id, uint64_t kernel_stack) {
    arch_x86_64_enable_sse();
    arch_x86_64_enable_paging_features();
    gdt_init_cpu(cpu_id);
    arch_trap_init_ap(cpu_id);
    tss_set_stack_for_cpu(cpu_id, kernel_stack);
    lapic_init_cpu();
}

void arch_platform_send_resched_ipi(uint32_t arch_cpu_id) {
    lapic_send_ipi(arch_cpu_id, INT_VECTOR_RESCHED);
}
