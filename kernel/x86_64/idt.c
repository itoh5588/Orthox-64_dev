#include "idt.h"
#include "x86_64/entry.h"
#include "x86_64/trap.h"
#include "gdt.h"
#include "lapic.h"
#include "net.h"
#include "task.h"
#include "spinlock.h"
#include <stdint.h>
#include <stddef.h>

void puts(const char *s);
void puthex(uint64_t v);

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

static uint64_t rdmsr_local(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static struct idt_entry idt[256];
static struct idt_ptr idtr;
static uint8_t exception_stacks[ORTHOX_MAX_CPUS][8192] __attribute__((aligned(16)));
static int idt_built = 0;
static uint8_t resched_ipi_seen[ORTHOX_MAX_CPUS];

static void append_str(char* buf, int* pos, int max, const char* s) {
    while (s && *s && *pos + 1 < max) {
        buf[(*pos)++] = *s++;
    }
}

static void append_dec(char* buf, int* pos, int max, uint64_t v) {
    char tmp[21];
    int n = 0;
    if (v == 0) {
        if (*pos + 1 < max) buf[(*pos)++] = '0';
        return;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n-- > 0 && *pos + 1 < max) {
        buf[(*pos)++] = tmp[n];
    }
}

extern void isr0();  extern void isr1();  extern void isr2();  extern void isr3();
extern void isr4();  extern void isr5();  extern void isr6();  extern void isr7();
extern void isr8();  extern void isr9();  extern void isr10(); extern void isr11();
extern void isr12(); extern void isr13(); extern void isr14(); extern void isr15();
extern void isr16(); extern void isr17(); extern void isr18(); extern void isr19();
extern void isr20(); extern void isr21();
extern void isr32();
extern void isr33();
extern void isr36();
extern void isr48();

void idt_set_gate(uint8_t num, void* handler, uint8_t ist, uint8_t type) {
    uint64_t addr = (uint64_t)handler;
    idt[num].offset_low = addr & 0xFFFF;
    idt[num].selector = GDT_KERNEL_CODE;
    idt[num].ist = ist;
    idt[num].type_attributes = type;
    idt[num].offset_mid = (addr >> 16) & 0xFFFF;
    idt[num].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[num].reserved = 0;
}

static void idt_build_once(void) {
    if (idt_built) return;

    idtr.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtr.base = (uint64_t)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    idt_set_gate(0,  isr0,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(1,  isr1,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(2,  isr2,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(3,  isr3,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(4,  isr4,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(5,  isr5,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(6,  isr6,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(7,  isr7,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(8,  isr8,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(9,  isr9,  1, IDT_GATE_INTERRUPT);
    idt_set_gate(10, isr10, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(11, isr11, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(12, isr12, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(13, isr13, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(14, isr14, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(15, isr15, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(16, isr16, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(17, isr17, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(18, isr18, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(19, isr19, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(20, isr20, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(21, isr21, 1, IDT_GATE_INTERRUPT);

    idt_set_gate(INT_VECTOR_TIMER,    isr32, 1, IDT_GATE_INTERRUPT);
    idt_set_gate(INT_VECTOR_KEYBOARD, isr33, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(INT_VECTOR_SERIAL,   isr36, 0, IDT_GATE_INTERRUPT);
    idt_set_gate(INT_VECTOR_RESCHED,  isr48, 1, IDT_GATE_INTERRUPT);
    idt_built = 1;
}

void idt_init_cpu(uint32_t cpu_id) {
    if (cpu_id >= ORTHOX_MAX_CPUS) cpu_id = 0;
    idt_build_once();
    tss_set_ist_for_cpu(cpu_id, 1, (uint64_t)&exception_stacks[cpu_id][sizeof(exception_stacks[cpu_id])]);
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

void idt_init(void) {
    idt_init_cpu(0);
}

void arch_interrupt_dispatch(arch_interrupt_frame_t* frame) {
    kernel_lock_enter();
    if (frame->int_no == INT_VECTOR_PAGE_FAULT) {
        extern void vmm_page_fault_handler(arch_interrupt_frame_t* frame);
        vmm_page_fault_handler(frame);
        kernel_lock_exit();
        return;
    }

    if (frame->int_no == INT_VECTOR_TIMER) {
        struct cpu_local* cpu = get_cpu_local();
        if (!cpu || cpu->cpu_id == 0) {
            net_poll();
        }
        lapic_timer_tick();
        task_on_timer_tick();
        lapic_eoi();
        kernel_lock_exit();
        return;
    }
    
    if (frame->int_no == INT_VECTOR_KEYBOARD) {
        extern void keyboard_handler(void);
        keyboard_handler();
        extern void pic_eoi(int irq);
        pic_eoi(1);
        kernel_lock_exit();
        return;
    }

    if (frame->int_no == INT_VECTOR_SERIAL) {
        extern void serial_handler(void);
        serial_handler();
        extern void pic_eoi(int irq);
        pic_eoi(4);
        kernel_lock_exit();
        return;
    }

    if (frame->int_no == INT_VECTOR_RESCHED) {
        struct cpu_local* cpu = get_cpu_local();
        if (cpu && cpu->cpu_id < ORTHOX_MAX_CPUS && !resched_ipi_seen[cpu->cpu_id]) {
            char buf[96];
            int pos = 0;
            resched_ipi_seen[cpu->cpu_id] = 1;
            append_str(buf, &pos, sizeof(buf), "[smp] cpu");
            append_dec(buf, &pos, sizeof(buf), cpu->cpu_id);
            append_str(buf, &pos, sizeof(buf), " resched IPI received\r\n");
            buf[pos] = '\0';
            puts(buf);
        }
        lapic_eoi();
        kernel_lock_exit();
        return;
    }

    puts("\r\n*** EXCEPTION OCCURRED ***\r\n");
    puts("Vector: "); puthex(frame->int_no);
    puts(", Error Code: "); puthex(frame->error_code);
    puts("\r\nRIP: 0x"); puthex(frame->rip);
    puts(", CS: 0x"); puthex(frame->cs);
    puts(", RFLAGS: 0x"); puthex(frame->rflags);
    puts("\r\nRSP: 0x"); puthex(frame->rsp);
    puts(", SS: 0x"); puthex(frame->ss);
    {
        struct cpu_local* cpu = get_cpu_local();
        struct task* current = get_current_task();
        puts("\r\nCPU: "); puthex(cpu ? (uint64_t)cpu->cpu_id : 0xFFFFFFFFFFFFFFFFULL);
        puts(", CUR: 0x"); puthex((uint64_t)(uintptr_t)current);
        puts(", PID: "); puthex(current ? (uint64_t)(uint32_t)current->pid : 0xFFFFFFFFFFFFFFFFULL);
        puts(", STATE: "); puthex(current ? (uint64_t)(uint32_t)current->state : 0xFFFFFFFFFFFFFFFFULL);
        puts(", GS: 0x"); puthex(rdmsr_local(MSR_GS_BASE));
        puts(", KGS: 0x"); puthex(rdmsr_local(MSR_KERNEL_GS_BASE));
    }
    puts("\r\nHALTING...\r\n");
    kernel_lock_exit();

    for (;;) __asm__("hlt");
}
