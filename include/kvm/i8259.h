#ifndef KVM__PIC_H
#define KVM__PIC_H

#include <kvm/devices.h>
#include <kvm/mutex.h>
#include "asm/kvm.h"

#define PIC_NUM_PINS 16
#define SELECT_PIC(irq)			\
	((irq) < 8 ? KVM_IRQCHIP_PIC_MASTER : KVM_IRQCHIP_PIC_SLAVE)

typedef u64	gpa_t;

struct kvm_kpic_state {
	u8 last_irr;    /* edge detection */
	u8 irr;         /* interrupt request register */
	u8 imr;         /* interrupt mask register */
	u8 isr;         /* interrupt service register */
	u8 priority_add;        /* highest irq priority */
	u8 irq_base;
	u8 read_reg_select;
	u8 poll;
	u8 special_mask;
	u8 init_state;
	u8 auto_eoi;
	u8 rotate_on_auto_eoi;
	u8 special_fully_nested_mode;
	u8 init4;               /* true if 4 byte init */
	u8 elcr;                /* PIIX edge/trigger selection */
	u8 elcr_mask;
	u8 isr_ack;     /* interrupt ack detection */
	struct kvm_pic *pics_state;
};

struct kvm_pic {
	struct device_header dev_hdr;
	struct mutex mutex;
	bool wakeup_needed;
	unsigned pending_acks;
	struct kvm *kvm;
	struct kvm_kpic_state pics[2];
	int output;
	void (*ack_notifier)(void *opaque, int irq);
	unsigned long irq_states[PIC_NUM_PINS];
};

int kvm_pic_init(struct kvm *kvm);
void kvm_pic_destroy(struct kvm *kvm);
int kvm_pic_read_irq(struct kvm *kvm);
void kvm_pic_update_irq(struct kvm *kvm);
int kvm_pic_set_irq(struct kvm_pic *vpic, int irq, int level);

#endif
