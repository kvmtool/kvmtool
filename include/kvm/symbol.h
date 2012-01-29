#ifndef KVM__SYMBOL_H
#define KVM__SYMBOL_H

#include <stddef.h>
#include <string.h>

struct kvm;

#define SYMBOL_DEFAULT_UNKNOWN "<unknown>"

#ifdef CONFIG_HAS_BFD
int symbol__init(struct kvm *kvm);
int symbol__exit(struct kvm *kvm);
char *symbol__lookup(struct kvm *kvm, unsigned long addr, char *sym, size_t size);
#else
static inline int symbol__init(struct kvm *kvm) { return 0; }
static inline char *symbol__lookup(struct kvm *kvm, unsigned long addr, char *sym, size_t size)
{
	char *s = strncpy(sym, SYMBOL_DEFAULT_UNKNOWN, size);
	sym[size - 1] = '\0';
	return s;
}
static inline int symbol__exit(struct kvm *kvm) { return 0; }
#endif

#endif /* KVM__SYMBOL_H */
