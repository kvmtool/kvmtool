#include <io.h>
#include <uart.h>
#define COM_TX_FIFO	((short) 0x3f8)
#define COM_RX_FIFO	((short) 0x3f8)
#define COM_RX_STATUS	((short) 0x3fd)

int uart_getchar(void) {
	while (!(inb(COM_RX_STATUS) & 0x01)) ;
	return (int)inb(COM_RX_FIFO);
}

void uart_putchar(int ch) {
	outb(COM_TX_FIFO, ch);
}
