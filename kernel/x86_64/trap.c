#include <stdint.h>
#include "x86_64/trap.h"
#include "idt.h"
#include "syscall.h"

void arch_trap_init_bsp(void) {
    idt_init();
    syscall_init();
}

void arch_trap_init_ap(uint32_t cpu_id) {
    idt_init_cpu(cpu_id);
    syscall_init_cpu();
}
