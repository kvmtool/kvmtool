#include "kvm/symbol.h"

#include "kvm/kvm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <bfd.h>

static bfd		*abfd;

void symbol__init(const char *vmlinux)
{
	if (!vmlinux)
		return;

	bfd_init();

	abfd		= bfd_openr(vmlinux, NULL);
}

static asymbol *lookup(asymbol **symbols, int nr_symbols, const char *symbol_name)
{
	int i;

	for (i = 0; i < nr_symbols; i++) {
		asymbol *symbol = symbols[i];

		if (!strcmp(bfd_asymbol_name(symbol), symbol_name))
			return symbol;
	}

	return NULL;
}

char *symbol__lookup(struct kvm *kvm, unsigned long addr, char *sym, size_t size)
{
	const char *filename;
	bfd_vma sym_offset;
	bfd_vma sym_start;
	asection *section;
	unsigned int line;
	const char *func;
	long symtab_size;
	asymbol *symbol;
	asymbol **syms;
	int nr_syms;
	char *s;

	if (!abfd)
		goto not_found;

	if (!bfd_check_format(abfd, bfd_object))
		goto not_found;

	symtab_size	= bfd_get_symtab_upper_bound(abfd);
	if (!symtab_size)
		goto not_found;

	syms		= malloc(symtab_size);
	if (!syms)
		goto not_found;

	nr_syms		= bfd_canonicalize_symtab(abfd, syms);

	section		= bfd_get_section_by_name(abfd, ".debug_aranges");
	if (!section)
		goto not_found;

	if (!bfd_find_nearest_line(abfd, section, NULL, addr, &filename, &func, &line))
		goto not_found;

	if (!func)
		goto not_found;

	symbol		= lookup(syms, nr_syms, func);
	if (!symbol)
		goto not_found;

	sym_start	= bfd_asymbol_value(symbol);

	sym_offset	= addr - sym_start;

	snprintf(sym, size, "%s+%llx (%s:%i)", func, (long long) sym_offset, filename, line);

	sym[size - 1] = '\0';

	free(syms);

	return sym;

not_found:
	s = strncpy(sym, "<unknown>", size);

	sym[size - 1] = '\0';

	return s;
}
