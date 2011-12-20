#ifndef KVM__STOP_H
#define KVM__STOP_H

#include <kvm/util.h>

int kvm_cmd_stop(int argc, const char **argv, const char *prefix);
void kvm_stop_help(void) NORETURN;

#endif
