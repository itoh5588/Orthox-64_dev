#include <stdint.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %b0, %w1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %w1, %b0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

void pic_init(void) {
    // ICW1: 初期化開始
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();

    // ICW2: ベクタのオフセット設定 (PIC1: 0x20=32, PIC2: 0x28=40)
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();

    // ICW3: カスケード設定
    outb(PIC1_DATA, 4); io_wait();
    outb(PIC2_DATA, 2); io_wait();

    // ICW4: 環境設定 (8086モード)
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    // マスク (IRQ1, IRQ4 のみを許可、他はマスク)
    // IRQ1: キーボード
    // IRQ4: シリアルポート
    outb(PIC1_DATA, ~( (1 << 1) | (1 << 4) ));
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(int irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_unmask_irq(int irq) {
    uint16_t port;
    uint8_t mask;
    if (irq < 0 || irq > 15) return;
    port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    mask = inb(port);
    mask &= (uint8_t)~(1U << (irq & 7));
    outb(port, mask);
    if (irq >= 8) {
        mask = inb(PIC1_DATA);
        mask &= (uint8_t)~(1U << 2);
        outb(PIC1_DATA, mask);
    }
}

void pic_mask_irq(int irq) {
    uint16_t port;
    uint8_t mask;
    if (irq < 0 || irq > 15) return;
    port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    mask = inb(port);
    mask |= (uint8_t)(1U << (irq & 7));
    outb(port, mask);
}
