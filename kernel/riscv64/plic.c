/*
 * SiFive PLIC ドライバ (QEMU virt)。
 *
 * レジスタ配置 (base = 0x0C00_0000):
 *   priority[irq]      base + 4*irq
 *   pending            base + 0x1000
 *   enable[ctx][irq]   base + 0x2000 + 0x80*ctx   (irq ごとに 1 bit)
 *   threshold[ctx]     base + 0x200000 + 0x1000*ctx
 *   claim/complete[ctx] base + 0x200004 + 0x1000*ctx
 *
 * コンテキスト番号は hart ごとに M モードと S モードで 2 つ割り当てられる。
 * hart h の S モードコンテキストは 2h+1。**CPU index ではなく物理 hart id**
 * から算出すること (OpenSBI が選ぶ boot hart は hart 0 とは限らない)。
 */

#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/plic.h"
#include "riscv64/trap.h"

#define PLIC_BASE          RISCV64_QEMU_VIRT_PLIC_BASE
#define PLIC_PRIORITY(irq) (PLIC_BASE + 4ULL * (irq))
#define PLIC_ENABLE(ctx)   (PLIC_BASE + 0x2000ULL + 0x80ULL * (ctx))
#define PLIC_THRESHOLD(ctx) (PLIC_BASE + 0x200000ULL + 0x1000ULL * (ctx))
#define PLIC_CLAIM(ctx)    (PLIC_BASE + 0x200004ULL + 0x1000ULL * (ctx))

/* PLIC が扱う割り込み番号の上限 (QEMU virt は 96 まで) */
#define PLIC_MAX_IRQ 96

static inline volatile uint32_t* plic_reg(uint64_t addr) {
    return (volatile uint32_t*)(uintptr_t)addr;
}

/* 現在の hart の S モードコンテキスト番号 */
static uint32_t plic_self_context(void) {
    uint64_t hartid = riscv64_smp_hartid((uint32_t)riscv64_current_hart_index());
    return (uint32_t)(2ULL * hartid + 1ULL);
}

void riscv64_plic_set_priority(uint32_t irq, uint32_t priority) {
    if (irq == 0 || irq >= PLIC_MAX_IRQ) return;
    *plic_reg(PLIC_PRIORITY(irq)) = priority;
}

void riscv64_plic_init_global(void) {
    /* 優先度 0 は「無効」なので、使う番号だけ 1 にする。
     * 残りは 0 のままにして、有効化ビットを立て忘れても発火しないようにする */
    *plic_reg(PLIC_PRIORITY(RISCV64_IRQ_UART0)) = 1;
}

void riscv64_plic_init_hart(void) {
    uint32_t ctx = plic_self_context();
    /* しきい値 0 = 優先度 1 以上をすべて通す */
    *plic_reg(PLIC_THRESHOLD(ctx)) = 0;
    /* 有効化ビットは全クリアしてから、必要なものだけ足す */
    for (uint32_t i = 0; i < (PLIC_MAX_IRQ + 31U) / 32U; i++) {
        plic_reg(PLIC_ENABLE(ctx))[i] = 0;
    }
}

void riscv64_plic_enable_irq(uint32_t irq) {
    uint32_t ctx = plic_self_context();
    if (irq == 0 || irq >= PLIC_MAX_IRQ) return;
    plic_reg(PLIC_ENABLE(ctx))[irq / 32U] |= (1U << (irq % 32U));
}

uint32_t riscv64_plic_claim(void) {
    return *plic_reg(PLIC_CLAIM(plic_self_context()));
}

void riscv64_plic_complete(uint32_t irq) {
    if (irq == 0) return;
    *plic_reg(PLIC_CLAIM(plic_self_context())) = irq;
}
