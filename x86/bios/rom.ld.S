OUTPUT_FORMAT("elf32-i386", "elf32-i386", "elf32-i386")
OUTPUT_ARCH(i386)

SECTIONS {
	.text 0 : {
		*(.text)
	}

	/DISCARD/ : {
		*(.debug*)
		*(.data)
		*(.bss)
		*(.eh_frame*)
	}
}

