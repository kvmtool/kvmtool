#ifndef TRAP_H
#define TRAP_H

// x86 trap and interrupt constants.
#include <stddef.h>
// Processor-defined:
#define T_DIVIDE				 0			// divide error
#define T_DEBUG					1			// debug exception
#define T_NMI						2			// non-maskable interrupt
#define T_BRKPT					3			// breakpoint
#define T_OFLOW					4			// overflow
#define T_BOUND					5			// bounds check
#define T_ILLOP					6			// illegal opcode
#define T_DEVICE				 7			// device not available
#define T_DBLFLT				 8			// double fault
// #define T_COPROC			9			// reserved (not used since 486)
#define T_TSS					 10			// invalid task switch segment
#define T_SEGNP				 11			// segment not present
#define T_STACK				 12			// stack exception
#define T_GPFLT				 13			// general protection fault
#define T_PGFLT				 14			// page fault
// #define T_RES				15			// reserved
#define T_FPERR				 16			// floating point error
#define T_ALIGN				 17			// aligment check
#define T_MCHK					18			// machine check
#define T_SIMDERR			 19			// SIMD floating point error

// These are arbitrarily chosen, but with care not to overlap
// processor defined exceptions or interrupt vectors.
#define T_SYSCALL			 64			// system call
#define T_DEFAULT			500			// catchall

#define T_IRQ0					32			// IRQ 0 corresponds to int T_IRQ

#define IRQ_TIMER				0
#define IRQ_KBD					1
#define IRQ_COM1				 4
#define IRQ_IDE				 14
#define IRQ_ERROR			 19
#define IRQ_SPURIOUS		31

// Layout of the trap frame built on the stack by the
// hardware and by trapasm.S, and passed to trap().
struct trapframe {
  // registers
  uint64_t rax;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rbp;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;

  // rest of trap frame
  // remove segment registers because the value is always same (2 for kernel, 4
  // for user) uint16_t gs; uint16_t padding_gs1; uint32_t padding_gs2; uint16_t
  // fs; uint16_t padding_fs1; uint32_t padding_fs2; uint16_t es; uint16_t
  // padding_es1; uint32_t padding_es2; uint16_t ds; uint16_t padding_ds1;
  // uint32_t padding_ds2;

  uint64_t trapno;

  // below here defined by x86-64 hardware
  uint64_t err;
  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp;
  uint64_t ss;
};

typedef struct trapframe trapframe_t __attribute__((aligned(16)));

struct gatedesc;

static inline void lidt(struct gatedesc *p, uint16_t size) {
	volatile uint16_t pd[5];

	pd[0] = size - 1;
	pd[1] = ((uint64_t)p) & 0xffff;
	pd[2] = (((uint64_t)p) >> 16) & 0xffff;
	pd[3] = (((uint64_t)p) >> 32) & 0xffff;
	pd[4] = (((uint64_t)p) >> 48) & 0xffff;

	__asm__ volatile("lidt (%0)" : : "r"(pd));
}

#endif
