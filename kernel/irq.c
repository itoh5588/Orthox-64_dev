#include "irq.h"
#include "idt.h"
#include "spinlock.h"

struct irq_slot {
    irq_handler_t handler;
    void* ctx;
};

static spinlock_t g_irq_lock;
static struct irq_slot g_legacy_irq_slots[16];
static struct irq_slot g_vector_irq_slots[256];
static int g_next_vector = INT_VECTOR_MSI_BASE;

int irq_register_legacy(int irq, irq_handler_t handler, void* ctx) {
    int ret = 0;
    uint64_t flags;
    if (irq < 0 || irq >= 16 || !handler) return -1;

    flags = spin_lock_irqsave(&g_irq_lock);
    if (g_legacy_irq_slots[irq].handler) {
        ret = -1;
    } else {
        g_legacy_irq_slots[irq].handler = handler;
        g_legacy_irq_slots[irq].ctx = ctx;
    }
    spin_unlock_irqrestore(&g_irq_lock, flags);
    return ret;
}

int irq_dispatch_legacy(int irq) {
    irq_handler_t handler;
    void* ctx;
    uint64_t flags;

    if (irq < 0 || irq >= 16) return 0;

    flags = spin_lock_irqsave(&g_irq_lock);
    handler = g_legacy_irq_slots[irq].handler;
    ctx = g_legacy_irq_slots[irq].ctx;
    spin_unlock_irqrestore(&g_irq_lock, flags);

    if (!handler) return 0;
    return handler(irq, ctx);
}

int irq_alloc_vector(void) {
    int vector = -1;
    uint64_t flags = spin_lock_irqsave(&g_irq_lock);
    if (g_next_vector < INT_VECTOR_MSI_END) {
        vector = g_next_vector++;
    }
    spin_unlock_irqrestore(&g_irq_lock, flags);
    return vector;
}

int irq_register_vector(int vector, irq_handler_t handler, void* ctx) {
    int ret = 0;
    uint64_t flags;
    if (vector < INT_VECTOR_MSI_BASE || vector >= INT_VECTOR_MSI_END || !handler) return -1;

    flags = spin_lock_irqsave(&g_irq_lock);
    if (g_vector_irq_slots[vector].handler) {
        ret = -1;
    } else {
        g_vector_irq_slots[vector].handler = handler;
        g_vector_irq_slots[vector].ctx = ctx;
    }
    spin_unlock_irqrestore(&g_irq_lock, flags);
    return ret;
}

int irq_dispatch_vector(int vector) {
    irq_handler_t handler;
    void* ctx;
    uint64_t flags;

    if (vector < 0 || vector >= 256) return 0;

    flags = spin_lock_irqsave(&g_irq_lock);
    handler = g_vector_irq_slots[vector].handler;
    ctx = g_vector_irq_slots[vector].ctx;
    spin_unlock_irqrestore(&g_irq_lock, flags);

    if (!handler) return 0;
    return handler(vector, ctx);
}
