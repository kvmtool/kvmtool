#ifndef _KVM_BARRIER_H_
#define _KVM_BARRIER_H_

/*
 * asm/system.h cannot be #included standalone on 32-bit x86 yet.
 *
 * Provide the dependencies here - we can drop these wrappers once
 * the header is fixed upstream:
 */
#include <stdbool.h>
#include <linux/stringify.h>
#include <linux/bitops.h>
#include <asm/alternative.h>
#include <asm/system.h>

#endif /* _KVM_BARRIER_H_ */
