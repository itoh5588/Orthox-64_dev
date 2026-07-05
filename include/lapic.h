#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

#define LAPIC_BASE_ADDR 0xFEE00000

// LAPIC レジスタオフセット
#define LAPIC_REG_EOI         0x0B0
#define LAPIC_REG_SVR         0x0F0
#define LAPIC_REG_ICR_LOW     0x300
#define LAPIC_REG_ICR_HIGH    0x310
#define LAPIC_REG_LVT_TIMER   0x320
#define LAPIC_REG_TIMER_INIT  0x380
#define LAPIC_REG_TIMER_CUR   0x390
#define LAPIC_REG_TIMER_DIV   0x3E0

// タイマー設定ビット
#define LAPIC_TIMER_PERIODIC  (1 << 17)
#define LAPIC_TIMER_DIV_16    0x03

// PIT 定数 (キャリブレーション用)
#define PIT_FREQ              1193182
#define SCHED_TICK_MS         10
#define PIT_COUNT_FOR_TICK    (PIT_FREQ / (1000 / SCHED_TICK_MS))

void lapic_init(void);
void lapic_init_cpu(void);
void lapic_eoi(void);
void lapic_timer_tick(void);
uint64_t lapic_get_ticks_ms(void);
uint64_t lapic_get_cpu_ticks_ms(uint32_t cpu_id);
void lapic_send_ipi(uint32_t lapic_id, uint8_t vector);

#endif
