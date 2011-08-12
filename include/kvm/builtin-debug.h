#ifndef KVM__DEBUG_H
#define KVM__DEBUG_H

int kvm_cmd_debug(int argc, const char **argv, const char *prefix);
void kvm_debug_help(void);

#endif
