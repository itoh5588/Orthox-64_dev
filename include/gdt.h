#ifndef GDT_H
#define GDT_H

#include <stdint.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

// 64bit TSS ディスクリプタ (16バイト)
struct tss_entry {
    uint16_t length;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  flags1;
    uint8_t  flags2;
    uint8_t  base_high_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

// TSS 本体の構造 (x86_64)
struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];    // rsp[0] が RSP0
    uint64_t reserved1;
    uint64_t ist[7];    // ist[0..6] が IST1..7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(void);
void gdt_init_cpu(uint32_t cpu_id);
void tss_set_stack(uint64_t stack);
void tss_set_stack_for_cpu(uint32_t cpu_id, uint64_t stack);
void tss_set_ist(int index, uint64_t stack);
void tss_set_ist_for_cpu(uint32_t cpu_id, int index, uint64_t stack);

#define GDT_KERNEL_CODE_IDX 1
#define GDT_KERNEL_DATA_IDX 2
#define GDT_USER_DATA_IDX   3
#define GDT_USER_CODE_IDX   4
#define GDT_TSS_IDX         5

#define GDT_KERNEL_CODE (GDT_KERNEL_CODE_IDX * 8)
#define GDT_KERNEL_DATA (GDT_KERNEL_DATA_IDX * 8)
#define GDT_USER_DATA   (GDT_USER_DATA_IDX * 8 + 3)
#define GDT_USER_CODE   (GDT_USER_CODE_IDX * 8 + 3)
#define GDT_TSS         (GDT_TSS_IDX * 8)

#define GDT_ENTRY_COUNT 7

#endif
