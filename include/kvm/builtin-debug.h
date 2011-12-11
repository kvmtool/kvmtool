#ifndef KVM__DEBUG_H
#define KVM__DEBUG_H

#include <linux/types.h>

struct debug_cmd {
	u32 type;
	u32 len;
	u32 dbg_type;
#define KVM_DEBUG_CMD_TYPE_DUMP	(1 << 0)
#define KVM_DEBUG_CMD_TYPE_NMI	(1 << 1)
	u32 cpu;
};

int kvm_cmd_debug(int argc, const char **argv, const char *prefix);
void kvm_debug_help(void);

#endif
