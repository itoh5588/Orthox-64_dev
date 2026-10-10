#ifndef ORTHOX_ARCH_RISCV64_AXI_INTC_H
#define ORTHOX_ARCH_RISCV64_AXI_INTC_H

#include <stdint.h>

/*
 * AMD AXI INTC (Arty S7 MicroBlaze V)。PLIC の代わりの外部割り込み。
 *
 * INTC の出力は M モード外部割り込み (MEIP) に入り、委譲できない。
 * fw が MEIE を閉じて SEIP を立てて渡してくるので、S モードで INTC を
 * 処理したあと SBI (Orthox EXTIRQ complete) で SEIP を下ろしてもらう。
 */

/* 割り込み入力 irq を有効にして INTC を動かす (boot hart で 1 回) */
void riscv64_axi_intc_init(uint64_t base, uint32_t irq);
/* scause = SEXT のときに呼ぶ。保留中の入力を処理して fw へ完了を返す */
void riscv64_axi_intc_handle(void);

#endif
