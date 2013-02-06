#ifndef KVM__GTK3_H
#define KVM__GTK3_H

#include "kvm/util.h"

struct framebuffer;

#ifdef CONFIG_HAS_GTK3
int kvm_gtk_init(struct kvm *kvm);
int kvm_gtk_exit(struct kvm *kvm);
#else
static inline int kvm_gtk_init(struct kvm *kvm)
{
	if (kvm->cfg.gtk)
		die("GTK3 support not compiled in. (install the gtk3-devel or libgtk3.0-dev package)");

	return 0;
}
static inline int kvm_gtk_exit(struct kvm *kvm)
{
	if (kvm->cfg.gtk)
		die("GTK3 support not compiled in. (install the gtk3-devel or libgtk3.0-dev package)");

	return 0;
}
#endif

#endif /* KVM__GTK3_H */
