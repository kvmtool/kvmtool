#include "kvm/bios.h"

#include "kvm/e820.h"

bioscall void int15_handler(struct biosregs *regs)
{
	switch (regs->eax) {
	case 0xe820:
		e820_query_map(regs);
		break;
	}
}
