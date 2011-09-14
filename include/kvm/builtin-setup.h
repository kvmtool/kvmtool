#ifndef KVM__SETUP_H
#define KVM__SETUP_H

int kvm_cmd_setup(int argc, const char **argv, const char *prefix);
void kvm_setup_help(void);
int kvm_setup_create_new(const char *guestfs_name);
void kvm_setup_resolv(const char *guestfs_name);

#endif
