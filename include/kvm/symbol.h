#ifndef KVM__SYMBOL_H
#define KVM__SYMBOL_H

#include <stddef.h>
#include <string.h>

struct kvm;

#ifdef CONFIG_HAS_BFD
void symbol__init(const char *vmlinux);
char *symbol__lookup(struct kvm *kvm, unsigned long addr, char *sym, size_t size);
#else
static inline void symbol__init(const char *vmlinux) { }
static inline char *symbol__lookup(struct kvm *kvm, unsigned long addr, char *sym, size_t size)
{
	char *s = strncpy(sym, "<unknown>", size);
	sym[size - 1] = '\0';
	return s;
}
#endif

#endif /* KVM__SYMBOL_H */
