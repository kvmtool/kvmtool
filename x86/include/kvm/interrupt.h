#ifndef KVM__INTERRUPT_H
#define KVM__INTERRUPT_H

#include <linux/types.h>
#include <kvm/devices.h>
#include <kvm/mutex.h>
#include "asm/kvm.h"
#include "kvm/bios.h"
#include "kvm/bios-export.h"

#define KVM_IRQCHIP_NUM_PINS_MAX	KVM_IOAPIC_NUM_PINS
#define SELECT_PIC(irq)			\
	((irq) < 8 ? KVM_IRQCHIP_PIC_MASTER : KVM_IRQCHIP_PIC_SLAVE)

bool irqchip_split(struct kvm *kvm);

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
	unsigned long irq_states[KVM_PIC_NUM_PINS];
};

int kvm_pic_init(struct kvm *kvm);
void kvm_pic_destroy(struct kvm *kvm);
int kvm_pic_read_irq(struct kvm *kvm);
void kvm_pic_update_irq(struct kvm *kvm);
int kvm_pic_set_irq(struct kvm_pic *vpic, int irq, int level);

struct real_intr_desc {
	u16 offset;
	u16 segment;
} __attribute__((packed));

#define REAL_SEGMENT_SHIFT	4
#define REAL_SEGMENT(addr)	((addr) >> REAL_SEGMENT_SHIFT)
#define REAL_OFFSET(addr)	((addr) & ((1 << REAL_SEGMENT_SHIFT) - 1))
#define REAL_INTR_SIZE		(REAL_INTR_VECTORS * sizeof(struct real_intr_desc))

struct interrupt_table {
	struct real_intr_desc entries[REAL_INTR_VECTORS];
};

void interrupt_table__copy(struct interrupt_table *itable, void *dst, unsigned int size);
void interrupt_table__setup(struct interrupt_table *itable, struct real_intr_desc *entry);
void interrupt_table__set(struct interrupt_table *itable, struct real_intr_desc *entry, unsigned int num);

#endif /* KVM__INTERRUPT_H */
