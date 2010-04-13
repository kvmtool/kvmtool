#include <kvm/bios.h>
#include <kvm/assembly.h>

	.org 0
#ifdef __x86_64__
	.code64
#else
	.code32
#endif
GLOBAL(intfake)
	.incbin "bios/intfake-real.bin"
GLOBAL(intfake_end)
