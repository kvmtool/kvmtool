#ifndef __I8254_H
#define __I8254_H

#include <kvm/devices.h>
#include <kvm/mutex.h>
#include "asm/kvm.h"

typedef u64	gpa_t;

#define ALIGN_DOWN(n, m) ((n) / (m) * (m))

struct kvm_pit;

struct kvm_kpit_channel_state {
	struct kvm_pit *pit;
	u32 count; /* can be 65536 */
	u16 latched_count;
	u8 count_latched;
	u8 status_latched;
	u8 status;
	u8 read_state;
	u8 write_state;
	u8 write_latch;
	u8 rw_mode;
	u8 mode;
	u8 bcd; /* not supported */
	u8 gate; /* timer start */
	s64 count_load_time;
	/* irq handling */
	s64 next_transition_time;
	u32 irq_disabled;
	timer_t timer;
	s64 cpu_clock_offset;
	s16 cpu_ticks_enabled;
	s64 remaining_time;
};

struct kvm_kpit_state {
	/* All members before "struct mutex lock" are protected by the lock. */
	struct kvm_kpit_channel_state channels[3];
	bool is_periodic;
	s64 period; 				/* unit: ns */

	struct mutex lock;
};

struct kvm_pit {
	struct device_header dev_hdr;
	struct kvm *kvm;
	struct kvm_kpit_state pit_state;
	int irq_source_id;
};

#define KVM_PIT_BASE_ADDRESS	    0x40
#define KVM_PIT_MEM_LENGTH	    4
#define PIT_FREQ 1193182
#define KVM_MAX_PIT_INTR_INTERVAL   HZ / 100
#define KVM_PIT_CHANNEL_MASK	    0x3

int pit_init(struct kvm *kvm);
void pit_destroy(struct kvm *kvm);

#endif