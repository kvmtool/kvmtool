OUTPUT_FORMAT("elf32-i386", "elf32-i386", "elf32-i386")
OUTPUT_ARCH(i386)

PHDRS {
	text PT_LOAD FLAGS(5);		/* R_E */
	data PT_LOAD FLAGS(7);		/* RWE */
	user PT_LOAD FLAGS(5);		/* R_E */
	percpu PT_LOAD FLAGS(6);	/* RW_ */
	init PT_LOAD FLAGS(7);		/* RWE */
	note PT_NOTE FLAGS(0);		/* ___ */
}

SECTIONS {
	. = 0;
	.text : { *(.text) } :text = 0x9090
}

