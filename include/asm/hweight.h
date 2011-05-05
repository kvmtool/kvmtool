#ifndef _KVM_ASM_HWEIGHT_H_
#define _KVM_ASM_HWEIGHT_H_

#include <linux/types.h>
unsigned int hweight32(unsigned int w);
unsigned long hweight64(__u64 w);

#endif /* _KVM_ASM_HWEIGHT_H_ */
