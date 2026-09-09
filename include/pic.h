#ifndef PIC_H
#define PIC_H

void pic_init(void);
void pic_eoi(int irq);
void pic_unmask_irq(int irq);
void pic_mask_irq(int irq);

#endif
