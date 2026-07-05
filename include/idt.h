#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IDT エントリ構造体 (16バイト)
struct idt_entry {
    uint16_t offset_low;    // オフセット 0..15
    uint16_t selector;      // コードセグメントセレクタ (GDT_KERNEL_CODE)
    uint8_t  ist;           // IST (Interrupt Stack Table) インデックス
    uint8_t  type_attributes; // 型と属性 (Present, DPL, Type)
    uint16_t offset_mid;    // オフセット 16..31
    uint32_t offset_high;   // オフセット 32..63
    uint32_t reserved;      // 予約済み (0)
} __attribute__((packed));

// IDTR ポインタ構造体
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// IDT の初期化
void idt_init(void);
void idt_init_cpu(uint32_t cpu_id);

// 割り込みゲートの設定
void idt_set_gate(uint8_t num, void* handler, uint8_t ist, uint8_t type);

// 割り込みベクタの定義
#define INT_VECTOR_PAGE_FAULT 14
#define INT_VECTOR_TIMER      32
#define INT_VECTOR_KEYBOARD   33
#define INT_VECTOR_SERIAL     36
#define INT_VECTOR_RESCHED    48
#define INT_VECTOR_MSI_BASE   49
#define INT_VECTOR_MSI_END    64
#define INT_VECTOR_PIC_BASE   32

// type_attributes の定義
#define IDT_GATE_INTERRUPT 0x8E // Present, Ring 0, Interrupt Gate
#define IDT_GATE_USER      0xEE // Present, Ring 3, Interrupt Gate (システムコール用)

#endif
