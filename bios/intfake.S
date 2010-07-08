#include <kvm/bios.h>
#include <kvm/assembly.h>

	.org 0
#ifdef CONFIG_X86_64
	.code64
#else
	.code32
#endif
GLOBAL(bios_intfake)
	.incbin "bios/intfake-real.bin"
GLOBAL(bios_intfake_end)
