
#include "kvm/devices.h"
#include "kvm/fdt.h"
#include "kvm/ioeventfd.h"
#include "kvm/ioport.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/irq.h"
#include "kvm/mutex.h"

#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/sizes.h>

/*
 * From the RISC-V Privlidged Spec v1.10:
 *
 * Global interrupt sources are assigned small unsigned integer identifiers,
 * beginning at the value 1.  An interrupt ID of 0 is reserved to mean no
 * interrupt.  Interrupt identifiers are also used to break ties when two or
 * more interrupt sources have the same assigned priority. Smaller values of
 * interrupt ID take precedence over larger values of interrupt ID.
 *
 * While the RISC-V supervisor spec doesn't define the maximum number of
 * devices supported by the PLIC, the largest number supported by devices
 * marked as 'riscv,plic0' (which is the only device type this driver supports,
 * and is the only extant PLIC as of now) is 1024.  As mentioned above, device
 * 0 is defined to be non-existant so this device really only supports 1023
 * devices.
 */

#define MAX_DEVICES	1024
#define MAX_CONTEXTS	15872

/*
 * The PLIC consists of memory-mapped control registers, with a memory map as
 * follows:
 *
 * base + 0x000000: Reserved (interrupt source 0 does not exist)
 * base + 0x000004: Interrupt source 1 priority
 * base + 0x000008: Interrupt source 2 priority
 * ...
 * base + 0x000FFC: Interrupt source 1023 priority
 * base + 0x001000: Pending 0
 * base + 0x001FFF: Pending
 * base + 0x002000: Enable bits for sources 0-31 on context 0
 * base + 0x002004: Enable bits for sources 32-63 on context 0
 * ...
 * base + 0x0020FC: Enable bits for sources 992-1023 on context 0
 * base + 0x002080: Enable bits for sources 0-31 on context 1
 * ...
 * base + 0x002100: Enable bits for sources 0-31 on context 2
 * ...
 * base + 0x1F1F80: Enable bits for sources 992-1023 on context 15871
 * base + 0x1F1F84: Reserved
 * ...		    (higher context IDs would fit here, but wouldn't fit
 *		     inside the per-context priority vector)
 * base + 0x1FFFFC: Reserved
 * base + 0x200000: Priority threshold for context 0
 * base + 0x200004: Claim/complete for context 0
 * base + 0x200008: Reserved
 * ...
 * base + 0x200FFC: Reserved
 * base + 0x201000: Priority threshold for context 1
 * base + 0x201004: Claim/complete for context 1
 * ...
 * base + 0xFFE000: Priority threshold for context 15871
 * base + 0xFFE004: Claim/complete for context 15871
 * base + 0xFFE008: Reserved
 * ...
 * base + 0xFFFFFC: Reserved
 */

/* Each interrupt source has a priority register associated with it. */
#define PRIORITY_BASE		0
#define PRIORITY_PER_ID		4

/*
 * Each hart context has a vector of interupt enable bits associated with it.
 * There's one bit for each interrupt source.
 */
#define ENABLE_BASE		0x2000
#define ENABLE_PER_HART		0x80

/*
 * Each hart context has a set of control registers associated with it.  Right
 * now there's only two: a source priority threshold over which the hart will
 * take an interrupt, and a register to claim interrupts.
 */
#define CONTEXT_BASE		0x200000
#define CONTEXT_PER_HART	0x1000
#define CONTEXT_THRESHOLD	0
#define CONTEXT_CLAIM		4

#define REG_SIZE		0x1000000

struct plic_state;

struct plic_context {
	/* State to which this belongs */
	struct plic_state *s;

	/* Static Configuration */
	u32 num;
	struct kvm_cpu *vcpu;

	/* Local IRQ state */
	struct mutex irq_lock;
	u8 irq_priority_threshold;
	u32 irq_enable[MAX_DEVICES/32];
	u32 irq_pending[MAX_DEVICES/32];
	u8 irq_pending_priority[MAX_DEVICES];
	u32 irq_claimed[MAX_DEVICES/32];
	u32 irq_autoclear[MAX_DEVICES/32];
};

struct plic_state {
	bool ready;
	struct kvm *kvm;

	/* Static Configuration */
	u32 num_irq;
	u32 num_irq_word;
	u32 max_prio;

	/* Context Array */
	u32 num_context;
	struct plic_context *contexts;

	/* Global IRQ state */
	struct mutex irq_lock;
	u8 irq_priority[MAX_DEVICES];
	u32 irq_level[MAX_DEVICES/32];
};

static struct plic_state plic;

/* Note: Must be called with c->irq_lock held */
static u32 __plic_context_best_pending_irq(struct plic_state *s,
					   struct plic_context *c)
{
	u8 best_irq_prio = 0;
	u32 i, j, irq, best_irq = 0;

	for (i = 0; i < s->num_irq_word; i++) {
		if (!c->irq_pending[i])
			continue;

		for (j = 0; j < 32; j++) {
			irq = i * 32 + j;
			if ((s->num_irq <= irq) ||
			    !(c->irq_pending[i] & (1 << j)) ||
			    (c->irq_claimed[i] & (1 << j)))
				continue;

			if (!best_irq ||
			    (best_irq_prio < c->irq_pending_priority[irq])) {
				best_irq = irq;
				best_irq_prio = c->irq_pending_priority[irq];
			}
		}
	}

	return best_irq;
}

/* Note: Must be called with c->irq_lock held */
static void __plic_context_irq_update(struct plic_state *s,
				      struct plic_context *c)
{
	u32 best_irq = __plic_context_best_pending_irq(s, c);
	u32 virq = (best_irq) ? KVM_INTERRUPT_SET : KVM_INTERRUPT_UNSET;

	if (ioctl(c->vcpu->vcpu_fd, KVM_INTERRUPT, &virq) < 0)
		pr_warning("KVM_INTERRUPT failed");
}

/* Note: Must be called with c->irq_lock held */
static u32 __plic_context_irq_claim(struct plic_state *s,
				    struct plic_context *c)
{
	u32 virq = KVM_INTERRUPT_UNSET;
	u32 best_irq = __plic_context_best_pending_irq(s, c);
	u32 best_irq_word = best_irq / 32;
	u32 best_irq_mask = (1 << (best_irq % 32));

	if (ioctl(c->vcpu->vcpu_fd, KVM_INTERRUPT, &virq) < 0)
		pr_warning("KVM_INTERRUPT failed");

	if (best_irq) {
		if (c->irq_autoclear[best_irq_word] & best_irq_mask) {
			c->irq_pending[best_irq_word] &= ~best_irq_mask;
			c->irq_pending_priority[best_irq] = 0;
			c->irq_claimed[best_irq_word] &= ~best_irq_mask;
			c->irq_autoclear[best_irq_word] &= ~best_irq_mask;
		} else
			c->irq_claimed[best_irq_word] |= best_irq_mask;
	}

	__plic_context_irq_update(s, c);

	return best_irq;
}

static void plic__irq_trig(struct kvm *kvm, int irq, int level, bool edge)
{
	bool irq_marked = false;
	u8 i, irq_prio, irq_word;
	u32 irq_mask;
	struct plic_context *c = NULL;
	struct plic_state *s = &plic;

	if (!s->ready)
		return;

	if (irq <= 0 || s->num_irq <= (u32)irq)
		goto done;

	mutex_lock(&s->irq_lock);

	irq_prio = s->irq_priority[irq];
	irq_word = irq / 32;
	irq_mask = 1 << (irq % 32);

	if (level)
		s->irq_level[irq_word] |= irq_mask;
	else
		s->irq_level[irq_word] &= ~irq_mask;

	/*
	 * Note: PLIC interrupts are level-triggered. As of now,
	 * there is no notion of edge-triggered interrupts. To
	 * handle this we auto-clear edge-triggered interrupts
	 * when PLIC context CLAIM register is read.
	 */
	for (i = 0; i < s->num_context; i++) {
		c = &s->contexts[i];

		mutex_lock(&c->irq_lock);
		if (c->irq_enable[irq_word] & irq_mask) {
			if (level) {
				c->irq_pending[irq_word] |= irq_mask;
				c->irq_pending_priority[irq] = irq_prio;
				if (edge)
					c->irq_autoclear[irq_word] |= irq_mask;
			} else {
				c->irq_pending[irq_word] &= ~irq_mask;
				c->irq_pending_priority[irq] = 0;
				c->irq_claimed[irq_word] &= ~irq_mask;
				c->irq_autoclear[irq_word] &= ~irq_mask;
			}
			__plic_context_irq_update(s, c);
			irq_marked = true;
		}
		mutex_unlock(&c->irq_lock);

		if (irq_marked)
			break;
	}

done:
	mutex_unlock(&s->irq_lock);
}

static void plic__priority_read(struct plic_state *s,
				u64 offset, void *data)
{
	u32 irq = (offset >> 2);

	if (irq == 0 || irq >= s->num_irq)
		return;

	mutex_lock(&s->irq_lock);
	ioport__write32(data, s->irq_priority[irq]);
	mutex_unlock(&s->irq_lock);
}

static void plic__priority_write(struct plic_state *s,
				 u64 offset, void *data)
{
	u32 val, irq = (offset >> 2);

	if (irq == 0 || irq >= s->num_irq)
		return;

	mutex_lock(&s->irq_lock);
	val = ioport__read32(data);
	val &= ((1 << PRIORITY_PER_ID) - 1);
	s->irq_priority[irq] = val;
	mutex_unlock(&s->irq_lock);
}

static void plic__context_enable_read(struct plic_state *s,
				      struct plic_context *c,
				      u64 offset, void *data)
{
	u32 irq_word = offset >> 2;

	if (s->num_irq_word < irq_word)
		return;

	mutex_lock(&c->irq_lock);
	ioport__write32(data, c->irq_enable[irq_word]);
	mutex_unlock(&c->irq_lock);
}

static void plic__context_enable_write(struct plic_state *s,
				       struct plic_context *c,
				       u64 offset, void *data)
{
	u8 irq_prio;
	u32 i, irq, irq_mask;
	u32 irq_word = offset >> 2;
	u32 old_val, new_val, xor_val;

	if (s->num_irq_word < irq_word)
		return;

	mutex_lock(&s->irq_lock);

	mutex_lock(&c->irq_lock);

	old_val = c->irq_enable[irq_word];
	new_val = ioport__read32(data);

	if (irq_word == 0)
		new_val &= ~0x1;

	c->irq_enable[irq_word] = new_val;

	xor_val = old_val ^ new_val;
	for (i = 0; i < 32; i++) {
		irq = irq_word * 32 + i;
		irq_mask = 1 << i;
		irq_prio = s->irq_priority[irq];
		if (!(xor_val & irq_mask))
			continue;
		if ((new_val & irq_mask) &&
		    (s->irq_level[irq_word] & irq_mask)) {
			c->irq_pending[irq_word] |= irq_mask;
			c->irq_pending_priority[irq] = irq_prio;
		} else if (!(new_val & irq_mask)) {
			c->irq_pending[irq_word] &= ~irq_mask;
			c->irq_pending_priority[irq] = 0;
			c->irq_claimed[irq_word] &= ~irq_mask;
		}
	}

	__plic_context_irq_update(s, c);

	mutex_unlock(&c->irq_lock);

	mutex_unlock(&s->irq_lock);
}

static void plic__context_read(struct plic_state *s,
			       struct plic_context *c,
			       u64 offset, void *data)
{
	mutex_lock(&c->irq_lock);

	switch (offset) {
	case CONTEXT_THRESHOLD:
		ioport__write32(data, c->irq_priority_threshold);
		break;
	case CONTEXT_CLAIM:
		ioport__write32(data, __plic_context_irq_claim(s, c));
		break;
	default:
		break;
	};

	mutex_unlock(&c->irq_lock);
}

static void plic__context_write(struct plic_state *s,
				struct plic_context *c,
				u64 offset, void *data)
{
	u32 val, irq_word, irq_mask;
	bool irq_update = false;

	mutex_lock(&c->irq_lock);

	switch (offset) {
	case CONTEXT_THRESHOLD:
		val = ioport__read32(data);
		val &= ((1 << PRIORITY_PER_ID) - 1);
		if (val <= s->max_prio)
			c->irq_priority_threshold = val;
		else
			irq_update = true;
		break;
	case CONTEXT_CLAIM:
		val = ioport__read32(data);
		irq_word = val / 32;
		irq_mask = 1 << (val % 32);
		if ((val < plic.num_irq) &&
		    (c->irq_enable[irq_word] & irq_mask)) {
			c->irq_claimed[irq_word] &= ~irq_mask;
			irq_update = true;
		}
		break;
	default:
		irq_update = true;
		break;
	};

	if (irq_update)
		__plic_context_irq_update(s, c);

	mutex_unlock(&c->irq_lock);
}

static void plic__mmio_callback(struct kvm_cpu *vcpu,
				u64 addr, u8 *data, u32 len,
				u8 is_write, void *ptr)
{
	u32 cntx;
	struct plic_state *s = ptr;

	if (len != 4)
		die("plic: invalid len=%d", len);

	addr &= ~0x3;
	addr -= RISCV_IRQCHIP;

	if (is_write) {
		if (PRIORITY_BASE <= addr && addr < ENABLE_BASE) {
			plic__priority_write(s, addr, data);
		} else if (ENABLE_BASE <= addr && addr < CONTEXT_BASE) {
			cntx = (addr - ENABLE_BASE) / ENABLE_PER_HART;
			addr -= cntx * ENABLE_PER_HART + ENABLE_BASE;
			if (cntx < s->num_context)
				plic__context_enable_write(s,
							   &s->contexts[cntx],
							   addr, data);
		} else if (CONTEXT_BASE <= addr && addr < REG_SIZE) {
			cntx = (addr - CONTEXT_BASE) / CONTEXT_PER_HART;
			addr -= cntx * CONTEXT_PER_HART + CONTEXT_BASE;
			if (cntx < s->num_context)
				plic__context_write(s, &s->contexts[cntx],
						    addr, data);
		}
	} else {
		if (PRIORITY_BASE <= addr && addr < ENABLE_BASE) {
			plic__priority_read(s, addr, data);
		} else if (ENABLE_BASE <= addr && addr < CONTEXT_BASE) {
			cntx = (addr - ENABLE_BASE) / ENABLE_PER_HART;
			addr -= cntx * ENABLE_PER_HART + ENABLE_BASE;
			if (cntx < s->num_context)
				plic__context_enable_read(s,
							  &s->contexts[cntx],
							  addr, data);
		} else if (CONTEXT_BASE <= addr && addr < REG_SIZE) {
			cntx = (addr - CONTEXT_BASE) / CONTEXT_PER_HART;
			addr -= cntx * CONTEXT_PER_HART + CONTEXT_BASE;
			if (cntx < s->num_context)
				plic__context_read(s, &s->contexts[cntx],
						   addr, data);
		}
	}
}

static void plic__generate_fdt_node(void *fdt, struct kvm *kvm)
{
	u32 i;
	char name[64];
	u32 reg_cells[4], *irq_cells;

	reg_cells[0] = 0;
	reg_cells[1] = cpu_to_fdt32(RISCV_IRQCHIP);
	reg_cells[2] = 0;
	reg_cells[3] = cpu_to_fdt32(RISCV_IRQCHIP_SIZE);

	irq_cells = calloc(plic.num_context * 2, sizeof(u32));
	if (!irq_cells)
		die("Failed to alloc irq_cells");

	sprintf(name, "interrupt-controller@%08x", (u32)RISCV_IRQCHIP);
	_FDT(fdt_begin_node(fdt, name));
	_FDT(fdt_property_string(fdt, "compatible", "riscv,plic0"));
	_FDT(fdt_property(fdt, "reg", reg_cells, sizeof(reg_cells)));
	_FDT(fdt_property_cell(fdt, "#interrupt-cells", 1));
	_FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
	_FDT(fdt_property_cell(fdt, "riscv,max-priority", plic.max_prio));
	_FDT(fdt_property_cell(fdt, "riscv,ndev", MAX_DEVICES - 1));
	_FDT(fdt_property_cell(fdt, "phandle", PHANDLE_PLIC));
	for (i = 0; i < (plic.num_context / 2); i++) {
		irq_cells[4*i + 0] = cpu_to_fdt32(PHANDLE_CPU_INTC_BASE + i);
		irq_cells[4*i + 1] = cpu_to_fdt32(0xffffffff);
		irq_cells[4*i + 2] = cpu_to_fdt32(PHANDLE_CPU_INTC_BASE + i);
		irq_cells[4*i + 3] = cpu_to_fdt32(9);
	}
	_FDT(fdt_property(fdt, "interrupts-extended", irq_cells,
			  sizeof(u32) * plic.num_context * 2));
	_FDT(fdt_end_node(fdt));

	free(irq_cells);
}

static int plic__init(struct kvm *kvm)
{
	u32 i;
	int ret;
	struct plic_context *c;

	if (riscv_irqchip != IRQCHIP_PLIC)
		return 0;

	plic.kvm = kvm;
	plic.num_irq = MAX_DEVICES;
	plic.num_irq_word = plic.num_irq / 32;
	if ((plic.num_irq_word * 32) < plic.num_irq)
		plic.num_irq_word++;
	plic.max_prio = (1UL << PRIORITY_PER_ID) - 1;

	plic.num_context = kvm->nrcpus * 2;
	plic.contexts = calloc(plic.num_context, sizeof(struct plic_context));
	if (!plic.contexts)
		return -ENOMEM;
	for (i = 0; i < plic.num_context; i++) {
		c = &plic.contexts[i];
		c->s = &plic;
		c->num = i;
		c->vcpu = kvm->cpus[i / 2];
		mutex_init(&c->irq_lock);
	}

	mutex_init(&plic.irq_lock);

	ret = kvm__register_mmio(kvm, RISCV_IRQCHIP, RISCV_IRQCHIP_SIZE,
				 false, plic__mmio_callback, &plic);
	if (ret)
		return ret;

	plic.ready = true;

	return 0;

}
dev_init(plic__init);

static int plic__exit(struct kvm *kvm)
{
	if (riscv_irqchip != IRQCHIP_PLIC)
		return 0;

	plic.ready = false;
	kvm__deregister_mmio(kvm, RISCV_IRQCHIP);
	free(plic.contexts);

	return 0;
}
dev_exit(plic__exit);

void plic__create(struct kvm *kvm)
{
	if (riscv_irqchip != IRQCHIP_UNKNOWN)
		return;

	riscv_irqchip = IRQCHIP_PLIC;
	riscv_irqchip_inkernel = false;
	riscv_irqchip_trigger = plic__irq_trig;
	riscv_irqchip_generate_fdt_node = plic__generate_fdt_node;
	riscv_irqchip_phandle = PHANDLE_PLIC;
	riscv_irqchip_msi_phandle = PHANDLE_RESERVED;
	riscv_irqchip_line_sensing = false;
}
