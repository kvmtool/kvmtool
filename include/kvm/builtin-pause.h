#ifndef KVM__PAUSE_H
#define KVM__PAUSE_H

int kvm_cmd_pause(int argc, const char **argv, const char *prefix);
void kvm_pause_help(void);

#endif
