/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KVM__KVM_CSR_H
#define KVM__KVM_CSR_H

#include <linux/const.h>

/* Scalar Crypto Extension - Entropy */
#define CSR_SEED		0x015
#define SEED_OPST_MASK		_AC(0xC0000000, UL)
#define SEED_OPST_BIST		_AC(0x00000000, UL)
#define SEED_OPST_WAIT		_AC(0x40000000, UL)
#define SEED_OPST_ES16		_AC(0x80000000, UL)
#define SEED_OPST_DEAD		_AC(0xC0000000, UL)
#define SEED_ENTROPY_MASK	_AC(0xFFFF, UL)

#endif /* KVM__KVM_CSR_H */
