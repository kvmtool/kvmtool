#ifndef IO_H
#define IO_H
#include <stddef.h>

static inline uint8_t inb(unsigned short port)
{
	uint8_t val;
	__asm__ __volatile__("inb %w1, %0":"=a"(val):"Nd"(port));
	return val;
}

static inline void outb(unsigned short port, uint8_t val)
{
	__asm__ volatile ("outb %0, %1"::"a" (val), "d"(port));
}

#endif
