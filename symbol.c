#include "kvm/symbol.h"

#include "kvm/kvm.h"

#include <linux/err.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <bfd.h>

static bfd *abfd;

int symbol_init(struct kvm *kvm)
{
	int ret = 0;

	if (!kvm->vmlinux)
		return 0;

	bfd_init();

	abfd = bfd_openr(kvm->vmlinux, NULL);
	if (abfd == NULL) {
		bfd_error_type err = bfd_get_error();

		switch (err) {
		case bfd_error_no_memory:
			ret = -ENOMEM;
			break;
		case bfd_error_invalid_target:
			ret = -EINVAL;
			break;
		default:
			ret = -EFAULT;
			break;
		}
	}

	return ret;
}
late_init(symbol_init);

static asymbol *lookup(asymbol **symbols, int nr_symbols, const char *symbol_name)
{
	int i, ret;

	ret = -ENOENT;

	for (i = 0; i < nr_symbols; i++) {
		asymbol *symbol = symbols[i];

		if (!strcmp(bfd_asymbol_name(symbol), symbol_name))
			return symbol;
	}

	return ERR_PTR(ret);
}

char *symbol_lookup(struct kvm *kvm, unsigned long addr, char *sym, size_t size)
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
	int nr_syms, ret;

	ret = -ENOENT;
	if (!abfd)
		goto not_found;

	if (!bfd_check_format(abfd, bfd_object))
		goto not_found;

	symtab_size = bfd_get_symtab_upper_bound(abfd);
	if (!symtab_size)
		goto not_found;

	ret = -ENOMEM;
	syms = malloc(symtab_size);
	if (!syms)
		goto not_found;

	nr_syms = bfd_canonicalize_symtab(abfd, syms);

	ret = -ENOENT;
	section = bfd_get_section_by_name(abfd, ".debug_aranges");
	if (!section)
		goto not_found;

	if (!bfd_find_nearest_line(abfd, section, NULL, addr, &filename, &func, &line))
		goto not_found;

	if (!func)
		goto not_found;

	symbol = lookup(syms, nr_syms, func);
	if (IS_ERR(symbol))
		goto not_found;

	sym_start = bfd_asymbol_value(symbol);

	sym_offset = addr - sym_start;

	snprintf(sym, size, "%s+%llx (%s:%i)", func, (long long) sym_offset, filename, line);

	sym[size - 1] = '\0';

	free(syms);

	return sym;

not_found:
	return ERR_PTR(ret);
}

int symbol_exit(struct kvm *kvm)
{
	bfd_boolean ret = TRUE;

	if (abfd)
		ret = bfd_close(abfd);

	if (ret == TRUE)
		return 0;

	return -EFAULT;
}
late_exit(symbol_exit);
