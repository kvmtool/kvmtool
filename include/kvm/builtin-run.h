#ifndef __KVM_RUN_H__
#define __KVM_RUN_H__

#include <kvm/util.h>

int kvm_cmd_run(int argc, const char **argv, const char *prefix);
void kvm_run_help(void) NORETURN;

void kvm_run_set_wrapper_sandbox(void);

#endif
