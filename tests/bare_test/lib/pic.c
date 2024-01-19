#include <io.h>
#include <stddef.h>
#include <pic.h>
#define PIC_MASTER	((short) 0x20)
#define PIC_SLAVE	((short) 0xa0)
#define PIC_ELCR	((short) 0x4d0)

#define PIC_MASTER_COMMAND	(PIC_MASTER)
#define PIC_MASTER_DATA	(PIC_MASTER + 1)
#define PIC_SLAVE_COMMAND	(PIC_SLAVE)
#define PIC_SLAVE_DATA	(PIC_SLAVE + 1)

void pic_mask(uint8_t IRQline) {
    unsigned short port;
    uint8_t value;

    if(IRQline < 8) {
        port = PIC_MASTER_DATA;
    } else {
        port = PIC_SLAVE_DATA;
        IRQline -= 8;
    }
    
    value = inb(port) | (1 << IRQline);
    outb(port, value);        
}

void pic_unmask(uint8_t IRQline) {
    unsigned short port;
    uint8_t value;

    if(IRQline < 8) {
        port = PIC_MASTER_DATA;
    } else {
        port = PIC_SLAVE_DATA;
        IRQline -= 8;
    }
    
    value = inb(port) & ~(1 << IRQline);
    outb(port, value);        
}

void pic_eoi(uint8_t IRQline) {
    if(IRQline >= 8){
        outb(PIC_SLAVE_COMMAND, 0x20);
    }

    outb(PIC_MASTER_COMMAND, 0x20);
}

void pic_init(uint8_t icw1, uint8_t irq_offset0, uint8_t irq_slave, uint8_t icw4, uint8_t irq_offset1) {
    // Mask all IRQs
    outb(PIC_MASTER_DATA, 0xff);
    outb(PIC_SLAVE_DATA, 0xff);
    
    // Set up master
    outb(PIC_MASTER_COMMAND, icw1);
    outb(PIC_MASTER_DATA, irq_offset0);
    if (!(icw1 & 0x02))
        outb(PIC_MASTER_DATA, 1 << irq_slave);
    if (icw1 & 0x01)
        outb(PIC_MASTER_DATA, icw4);

    if (!(icw1 & 0x02)) {
        // Set up slave
        outb(PIC_SLAVE_COMMAND, icw1);
        outb(PIC_SLAVE_DATA, irq_offset1);
        outb(PIC_SLAVE_DATA, irq_slave);
        if (icw1 & 0x01)
            outb(PIC_SLAVE_DATA, icw4);

        // unmask slave
        pic_unmask(0x04);
    }
}
