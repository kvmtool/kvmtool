#ifndef INTERRUPT_H
#define INTERRUPT_H
#include <traps.h>

void idtinit(void);
void tvinit(void);
void trap(struct trapframe *tf);
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }

#endif
