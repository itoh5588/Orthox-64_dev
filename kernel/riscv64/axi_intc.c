#include <stdint.h>
#include "riscv64/axi_intc.h"
#include "riscv64/boot.h"
#include "riscv64/sbi.h"

#define INTC_ISR 0x00U   /* 保留 (エッジ入力はここにラッチされる) */
#define INTC_IER 0x08U
#define INTC_IAR 0x0CU   /* 1 を書いたビットの ISR を落とす */
#define INTC_MER 0x1CU
#define INTC_MER_ME  0x1U
#define INTC_MER_HIE 0x2U

static uint64_t g_intc_base;
static uint32_t g_intc_uart_mask;

static inline volatile uint32_t* intc_reg(uint32_t off) {
    return (volatile uint32_t*)(uintptr_t)(g_intc_base + off);
}

void riscv64_axi_intc_init(uint64_t base, uint32_t irq) {
    g_intc_base = base;
    g_intc_uart_mask = 1U << irq;
    *intc_reg(INTC_IER) = g_intc_uart_mask;
    *intc_reg(INTC_MER) = INTC_MER_ME | INTC_MER_HIE;
}

void riscv64_axi_intc_handle(void) {
    uint32_t pending;
    if (!g_intc_base) return;
    pending = *intc_reg(INTC_ISR) & *intc_reg(INTC_IER);
    /* **ack は読み出しより先。**UART Lite の割り込みはエッジで ISR に
     * ラッチされる。読み出しの後に ack すると、読んでいる間に届いた
     * 文字のエッジまで消してしまい、次の割り込みが来ない */
    if (pending) *intc_reg(INTC_IAR) = pending;
    if (pending & g_intc_uart_mask) {
        riscv64_console_poll_input();
    }
    (void)riscv64_sbi_extirq_complete();
}
