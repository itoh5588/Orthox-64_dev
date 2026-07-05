#ifndef IRQ_H
#define IRQ_H

typedef int (*irq_handler_t)(int irq, void* ctx);

int irq_register_legacy(int irq, irq_handler_t handler, void* ctx);
int irq_dispatch_legacy(int irq);
int irq_alloc_vector(void);
int irq_register_vector(int vector, irq_handler_t handler, void* ctx);
int irq_dispatch_vector(int vector);

#endif
