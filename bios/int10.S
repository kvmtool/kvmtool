#include <kvm/bios.h>
#include <kvm/assembly.h>

	.org 0
#ifdef __x86_64__
	.code64
#else
	.code32
#endif
GLOBAL(int10)
	.incbin "bios/int10-real.bin"
GLOBAL(int10_end)
