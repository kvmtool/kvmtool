#ifndef KVM__DEBUG_H
#define KVM__DEBUG_H

#include <kvm/util.h>
#include <linux/types.h>

#define KVM_DEBUG_CMD_TYPE_DUMP	(1 << 0)
#define KVM_DEBUG_CMD_TYPE_NMI	(1 << 1)
#define KVM_DEBUG_CMD_TYPE_SYSRQ (1 << 2)

struct debug_cmd_params {
	u32 dbg_type;
	u32 cpu;
	char sysrq;
};

int kvm_cmd_debug(int argc, const char **argv, const char *prefix);
void kvm_debug_help(void) NORETURN;

#endif
