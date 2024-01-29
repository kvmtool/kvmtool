#ifndef PIC_H
#define PIC_H

#include <stddef.h>

void pic_mask(uint8_t IRQline);
void pic_unmask(uint8_t IRQline);
void pic_eoi(uint8_t IRQline);
void pic_init(uint8_t icw1, uint8_t irq_offset0, uint8_t irq_slave, uint8_t icw4, uint8_t irq_offset1);

#endif
