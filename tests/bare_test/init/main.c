#include <stdio.h>
#include <pic.h>
#include <pit.h>
#include <interrupt.h>

int main(void){
    kprintf("In Long Mode\n");
    
    // Enable Interrupts
    tvinit();
    idtinit();
    sti();

    pic_init(0x11, 0x20, 0x4, 0x3, 0x28);
    pic_unmask(0);
    pit_set(2, 0, 1000);
    
    while (1);
}
