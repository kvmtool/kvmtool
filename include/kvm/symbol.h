#ifndef KVM__SYMBOL_H
#define KVM__SYMBOL_H

#include <stddef.h>

struct kvm;

void symbol__init(const char *vmlinux);

char *symbol__lookup(struct kvm *kvm, unsigned long addr, char *sym, size_t size);

#endif /* KVM__SYMBOL_H */
