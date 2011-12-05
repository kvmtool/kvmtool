#ifndef __KVM_RUN_H__
#define __KVM_RUN_H__

int kvm_cmd_run(int argc, const char **argv, const char *prefix);
void kvm_run_help(void);

void kvm_run_set_wrapper_sandbox(void);

#endif
