#ifndef KVM__BALLOON_H
#define KVM__BALLOON_H

int kvm_cmd_balloon(int argc, const char **argv, const char *prefix);
void kvm_balloon_help(void);

#endif
