#ifndef KVM__RESUME_H
#define KVM__RESUME_H

#include <kvm/util.h>

int kvm_cmd_resume(int argc, const char **argv, const char *prefix);
void kvm_resume_help(void) NORETURN;

#endif
