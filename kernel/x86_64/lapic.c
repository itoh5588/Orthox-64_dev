#include <stdint.h>
#include "lapic.h"
#include "vmm.h"
#include "idt.h"
#include "task.h"

static volatile uint32_t* lapic_base;
static volatile uint64_t g_ticks_ms = 0;
static volatile uint64_t g_ticks_ms_per_cpu[ORTHOX_MAX_CPUS];
static volatile uint8_t g_timer_seen[ORTHOX_MAX_CPUS];
static uint32_t g_timer_ticks_per_interval = 0;

extern void puts(const char* s);

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

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %b0, %w1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %w1, %b0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static void lapic_write(uint32_t reg, uint32_t data) {
    lapic_base[reg >> 2] = data;
}

static uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg >> 2];
}

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector) {
    if (!lapic_base) return;
    lapic_write(LAPIC_REG_ICR_HIGH, lapic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, vector);
    while (lapic_read(LAPIC_REG_ICR_LOW) & (1U << 12)) {
        __asm__ volatile("pause");
    }
}

void lapic_eoi(void) {
    lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_timer_tick(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (cpu && cpu->cpu_id < ORTHOX_MAX_CPUS) {
        uint32_t cpu_id = cpu->cpu_id;
        if (cpu_id == 0) {
            g_ticks_ms += SCHED_TICK_MS;
        }
        g_ticks_ms_per_cpu[cpu_id] += SCHED_TICK_MS;
        if (cpu_id != 0 && !g_timer_seen[cpu_id]) {
            char buf[96];
            int pos = 0;
            g_timer_seen[cpu_id] = 1;
            append_str(buf, &pos, sizeof(buf), "[smp] cpu");
            append_dec(buf, &pos, sizeof(buf), cpu_id);
            append_str(buf, &pos, sizeof(buf), " local timer online\r\n");
            buf[pos] = '\0';
            puts(buf);
        }
    }
}

uint64_t lapic_get_ticks_ms(void) {
    return g_ticks_ms;
}

uint64_t lapic_get_cpu_ticks_ms(uint32_t cpu_id) {
    if (cpu_id >= ORTHOX_MAX_CPUS) return 0;
    return g_ticks_ms_per_cpu[cpu_id];
}

// PIT (Programmable Interval Timer) を使用した一時的な待機
static void pit_prepare_sleep(uint16_t count) {
    outb(0x43, 0x34);
    outb(0x40, count & 0xFF);
    outb(0x40, (count >> 8) & 0xFF);
}

static void pit_wait(void) {
    while (1) {
        outb(0x43, 0xE2);
        if (inb(0x40) & 0x80) break;
    }
}

void lapic_init(void) {
    lapic_base = (uint32_t*)PHYS_TO_VIRT(LAPIC_BASE_ADDR);

    // Spurious Interrupt Vector Register の設定
    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | 0x1FF);

    // タイマーのキャリブレーション (PIT を使用して 10ms の LAPIC ticks を測定)
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    pit_prepare_sleep(PIT_COUNT_FOR_TICK);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);
    
    pit_wait();
    
    g_timer_ticks_per_interval = 0xFFFFFFFF - lapic_read(LAPIC_REG_TIMER_CUR);

    // 定期的な割り込みの設定
    lapic_write(LAPIC_REG_LVT_TIMER, INT_VECTOR_TIMER | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_INIT, g_timer_ticks_per_interval);
}

void lapic_init_cpu(void) {
    if (!lapic_base) {
        lapic_base = (uint32_t*)PHYS_TO_VIRT(LAPIC_BASE_ADDR);
    }

    lapic_write(LAPIC_REG_SVR, lapic_read(LAPIC_REG_SVR) | 0x1FF);
    lapic_write(LAPIC_REG_LVT_TIMER, INT_VECTOR_TIMER | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    if (g_timer_ticks_per_interval) {
        lapic_write(LAPIC_REG_TIMER_INIT, g_timer_ticks_per_interval);
    }
}
