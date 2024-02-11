#include "kvm/i8259.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu-arch.h"

#define pr_pic_unimpl(fmt, ...)	\
	pr_err("pic: " fmt, ## __VA_ARGS__)

static void pic_irq_request(struct kvm *kvm, int level);

static void kvm_set_irq(struct kvm *kvm, int level)
{
	struct kvm_cpu *vcpu0 = kvm->cpus[0];

	if (level) {
		vcpu0->interrupt_request |= CPU_INTERRUPT_HARD;
		if (!pthread_equal(pthread_self(), vcpu0->thread)) {        
			int err = pthread_kill(vcpu0->thread, SIGUSR2);
			if (err && err != ESRCH) {
				fprintf(stderr, "send signal:%s: %s", __func__, strerror(err));
				exit(1);
			}
		}
	}else {
		vcpu0->interrupt_request &= ~CPU_INTERRUPT_HARD;
	}
}

static void pic_lock(struct kvm_pic *vpic)
{
	mutex_lock(&vpic->mutex);
}

static void pic_unlock(struct kvm_pic *vpic)
{
	bool wakeup = vpic->wakeup_needed;
	struct kvm *kvm = vpic->kvm;

	vpic->wakeup_needed = false;

	mutex_unlock(&vpic->mutex);
	if (wakeup) {
		kvm_set_irq(kvm, vpic->output);
	}
}

static void pic_clear_isr(struct kvm_kpic_state *s, int irq)
{
	s->isr &= ~(1 << irq);
}

/*
 * set irq level. If an edge is detected, then the IRR is set to 1
 */
static inline int pic_set_irq1(struct kvm_kpic_state *s, int irq, int level)
{
	int mask, ret = 1;
	mask = 1 << irq;
	if (s->elcr & mask)	/* level triggered */
		if (level) {
			ret = !(s->irr & mask);
			s->irr |= mask;
			s->last_irr |= mask;
		} else {
			s->irr &= ~mask;
			s->last_irr &= ~mask;
		}
	else	/* edge triggered */
		if (level) {
			if ((s->last_irr & mask) == 0) {
				ret = !(s->irr & mask);
				s->irr |= mask;
			}
			s->last_irr |= mask;
		} else
			s->last_irr &= ~mask;

	return (s->imr & mask) ? -1 : ret;
}

/*
 * return the highest priority found in mask (highest = smallest
 * number). Return 8 if no irq
 */
static inline int get_priority(struct kvm_kpic_state *s, int mask)
{
	int priority;
	if (mask == 0)
		return 8;
	priority = 0;
	while ((mask & (1 << ((priority + s->priority_add) & 7))) == 0)
		priority++;
	return priority;
}

/*
 * return the pic wanted interrupt. return -1 if none
 */
static int pic_get_irq(struct kvm_kpic_state *s)
{
	int mask, cur_priority, priority;

	mask = s->irr & ~s->imr;
	priority = get_priority(s, mask);
	if (priority == 8)
		return -1;
	/*
	 * compute current priority. If special fully nested mode on the
	 * master, the IRQ coming from the slave is not taken into account
	 * for the priority computation.
	 */
	mask = s->isr;
	if (s->special_fully_nested_mode && s == &s->pics_state->pics[0])
		mask &= ~(1 << 2);
	cur_priority = get_priority(s, mask);
	if (priority < cur_priority)
		/*
		 * higher priority found: an irq should be generated
		 */
		return (priority + s->priority_add) & 7;
	else
		return -1;
}

/*
 * raise irq to CPU if necessary. must be called every time the active
 * irq may change
 */
static void pic_update_irq(struct kvm_pic *s)
{
	int irq2, irq;

	irq2 = pic_get_irq(&s->pics[1]);
	if (irq2 >= 0) {
		/*
		 * if irq request by slave pic, signal master PIC
		 */
		pic_set_irq1(&s->pics[0], 2, 1);
		pic_set_irq1(&s->pics[0], 2, 0);
	}
	irq = pic_get_irq(&s->pics[0]);
	pic_irq_request(s->kvm, irq >= 0);
}

int kvm_pic_set_irq(struct kvm_pic *vpic, int irq, int level)
{
	int ret;

	BUG_ON(irq < 0 || irq >= PIC_NUM_PINS);

	pic_lock(vpic);
	ret = pic_set_irq1(&vpic->pics[irq >> 3], irq & 7, level);
	pic_update_irq(vpic);
	pic_unlock(vpic);

	return ret;
}

/*
 * acknowledge interrupt 'irq'
 */
static inline void pic_intack(struct kvm_kpic_state *s, int irq)
{
	s->isr |= 1 << irq;
	/*
	 * We don't clear a level sensitive interrupt here
	 */
	if (!(s->elcr & (1 << irq)))
		s->irr &= ~(1 << irq);

	if (s->auto_eoi) {
		if (s->rotate_on_auto_eoi)
			s->priority_add = (irq + 1) & 7;
		pic_clear_isr(s, irq);
	}

}

int pic_read_irq(struct kvm *kvm)
{
	int irq, irq2, intno;
	struct kvm_pic *s = kvm->arch.vpic;

	s->output = 0;

	// pic_lock(s);
	irq = pic_get_irq(&s->pics[0]);
	if (irq >= 0) {
		pic_intack(&s->pics[0], irq);
		if (irq == 2) {
			irq2 = pic_get_irq(&s->pics[1]);
			if (irq2 >= 0)
				pic_intack(&s->pics[1], irq2);
			else
				/*
				 * spurious IRQ on slave controller
				 */
				irq2 = 7;
			intno = s->pics[1].irq_base + irq2;
		} else
			intno = s->pics[0].irq_base + irq;
	} else {
		/*
		 * spurious IRQ on host controller
		 */
		irq = 7;
		intno = s->pics[0].irq_base + irq;
	}
	pic_update_irq(s);
	// pic_unlock(s);

	return intno;
}

static void kvm_pic_reset(struct kvm_kpic_state *s)
{
	s->last_irr = 0;
	s->irr &= s->elcr;
	s->imr = 0;
	s->isr = 0;
	s->priority_add = 0;
	s->poll = 0;
	s->special_mask = 0;
	s->rotate_on_auto_eoi = 0;
	s->read_reg_select = 0;
	if (!s->init4) {
		s->special_fully_nested_mode = 0;
		s->auto_eoi = 0;
	}
	s->init_state = 1;
}

static void pic_ioport_write(void *opaque, u32 addr, u32 val)
{
	struct kvm_kpic_state *s = opaque;
	int priority, cmd, irq;

	addr &= 1;
	if (addr == 0) {
		if (val & 0x10) {
			s->init4 = val & 1;
			if (val & 0x02)
				pr_pic_unimpl("single mode not supported");
			if (val & 0x08)
				pr_pic_unimpl(
						"level sensitive irq not supported");
			kvm_pic_reset(s);
		} else if (val & 0x08) {
			if (val & 0x04)
				s->poll = 1;
			if (val & 0x02)
				s->read_reg_select = val & 1;
			if (val & 0x40)
				s->special_mask = (val >> 5) & 1;
		} else {
			cmd = val >> 5;
			switch (cmd) {
			case 0:
			case 4:
				s->rotate_on_auto_eoi = cmd >> 2;
				break;
			case 1:	/* end of interrupt */
			case 5:
				priority = get_priority(s, s->isr);
				if (priority != 8) {
					irq = (priority + s->priority_add) & 7;
					if (cmd == 5)
						s->priority_add = (irq + 1) & 7;
					pic_clear_isr(s, irq);
					pic_update_irq(s->pics_state);
				}
				break;
			case 3:
				irq = val & 7;
				pic_clear_isr(s, irq);
				pic_update_irq(s->pics_state);
				break;
			case 6:
				s->priority_add = (val + 1) & 7;
				pic_update_irq(s->pics_state);
				break;
			case 7:
				irq = val & 7;
				s->priority_add = (irq + 1) & 7;
				pic_clear_isr(s, irq);
				pic_update_irq(s->pics_state);
				break;
			default:
				break;	/* no operation */
			}
		}
	} else
		switch (s->init_state) {
		case 0: { /* normal mode */
			// u8 imr_diff = s->imr ^ val,
			//         off = (s == &s->pics_state->pics[0]) ? 0 : 8;
			s->imr = val;
			// for (irq = 0; irq < PIC_NUM_PINS/2; irq++)
			//         if (imr_diff & (1 << irq))
			//                kvm_fire_mask_notifiers(
			//                        s->pics_state->kvm,
			//                        SELECT_PIC(irq + off),
			//                        irq + off,
			//                        !!(s->imr & (1 << irq)));
			pic_update_irq(s->pics_state);
			break;
		}
		case 1:
			s->irq_base = val & 0xf8;
			s->init_state = 2;
			break;
		case 2:
			if (s->init4)
				s->init_state = 3;
			else
				s->init_state = 0;
			break;
		case 3:
			s->special_fully_nested_mode = (val >> 4) & 1;
			s->auto_eoi = (val >> 1) & 1;
			s->init_state = 0;
			break;
		}
}

static u32 pic_poll_read(struct kvm_kpic_state *s, u32 addr1)
{
	int ret;

	ret = pic_get_irq(s);
	if (ret >= 0) {
		if (addr1 >> 7) {
			s->pics_state->pics[0].isr &= ~(1 << 2);
			s->pics_state->pics[0].irr &= ~(1 << 2);
		}
		s->irr &= ~(1 << ret);
		pic_clear_isr(s, ret);
		if (addr1 >> 7 || ret != 2)
			pic_update_irq(s->pics_state);
		/* Bit 7 is 1, means there's an interrupt */
		ret |= 0x80;
	} else {
		/* Bit 7 is 0, means there's no interrupt */
		ret = 0x07;
		pic_update_irq(s->pics_state);
	}

	return ret;
}

static u32 pic_ioport_read(void *opaque, u32 addr)
{
	struct kvm_kpic_state *s = opaque;
	int ret;

	if (s->poll) {
		ret = pic_poll_read(s, addr);
		s->poll = 0;
	} else
		if ((addr & 1) == 0)
			if (s->read_reg_select)
				ret = s->isr;
			else
				ret = s->irr;
		else
			ret = s->imr;
	return ret;
}

static void elcr_ioport_write(void *opaque, u32 val)
{
	struct kvm_kpic_state *s = opaque;
	s->elcr = val & s->elcr_mask;
}

static u32 elcr_ioport_read(void *opaque)
{
	struct kvm_kpic_state *s = opaque;
	return s->elcr;
}

static int picdev_write(struct kvm_pic *s,
			 gpa_t addr, int len, const void *val)
{
	unsigned char data = *(unsigned char *)val;

	if (len != 1) {
		pr_pic_unimpl("non byte write\n");
		return 0;
	}
	switch (addr) {
	case 0x20:
	case 0x21:
		pic_lock(s);
		pic_ioport_write(&s->pics[0], addr, data);
		pic_unlock(s);
		break;
	case 0xa0:
	case 0xa1:
		pic_lock(s);
		pic_ioport_write(&s->pics[1], addr, data);
		pic_unlock(s);
		break;
	case 0x4d0:
	case 0x4d1:
		pic_lock(s);
		elcr_ioport_write(&s->pics[addr & 1], data);
		pic_unlock(s);
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int picdev_read(struct kvm_pic *s,
			   gpa_t addr, int len, void *val)
{
	unsigned char *data = (unsigned char *)val;

	if (len != 1) {
		memset(val, 0, len);
		pr_pic_unimpl("non byte read\n");
		return 0;
	}
	switch (addr) {
	case 0x20:
	case 0x21:
	case 0xa0:
	case 0xa1:
		pic_lock(s);
		*data = pic_ioport_read(&s->pics[addr >> 7], addr);
		pic_unlock(s);
		break;
	case 0x4d0:
	case 0x4d1:
		pic_lock(s);
		*data = elcr_ioport_read(&s->pics[addr & 1]);
		pic_unlock(s);
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

/*
 * callback when PIC0 irq status changed
 */
static void pic_irq_request(struct kvm *kvm, int level)
{
	struct kvm_pic *vpic = kvm->arch.vpic;

	if (!vpic->output)
		vpic->wakeup_needed = true;
	vpic->output = level;
}


static void kvm_pic_io_handler(struct kvm_cpu *vpcu, u64 addr, u8 *data,
				u32 len, u8 is_write, void *ptr)
{
	if (is_write)
		picdev_write(ptr, addr, len, data);
	else
		picdev_read(ptr, addr, len, data);

	// TODO: error handle
}

int kvm_pic_init(struct kvm *kvm)
{
	struct kvm_pic *vpic;
	int ret;

	vpic = malloc(sizeof(struct kvm_pic));
	if (!vpic)
		return -ENOMEM;

	memset(vpic, 0, sizeof(struct kvm_pic));
	mutex_init(&vpic->mutex);
	vpic->kvm = kvm;
	vpic->pics[0].elcr_mask = 0xf8;
	vpic->pics[1].elcr_mask = 0xde;
	vpic->pics[0].pics_state = vpic;
	vpic->pics[1].pics_state = vpic;

	vpic->dev_hdr.bus_type = DEVICE_BUS_IOPORT;
	vpic->dev_hdr.data = vpic;
	ret = device__register(&vpic->dev_hdr);
	if (ret < 0)
		goto fail_device;
	// master
	ret = kvm__register_pio(kvm, 0x20, 2, kvm_pic_io_handler, vpic);
	if (ret < 0)
		goto fail_reg;
	// slave
	ret = kvm__register_pio(kvm, 0xa0, 2, kvm_pic_io_handler, vpic);
	if (ret < 0)
		goto fail_reg2;
	// elcr
	ret = kvm__register_pio(kvm, 0x4d0, 2, kvm_pic_io_handler, vpic);
	if (ret < 0)
		goto fail_reg3;

	kvm->arch.vpic = vpic;
	return 0;

fail_reg3:
	kvm__deregister_pio(kvm, 0xa0);
fail_reg2:
	kvm__deregister_pio(kvm, 0x20);
fail_reg:
	device__unregister(&vpic->dev_hdr);
fail_device:
	free(vpic);
	return ret;
}

void kvm_pic_destroy(struct kvm *kvm)
{
	struct kvm_pic *vpic = kvm->arch.vpic;

	if (!vpic)
		return;

	kvm__deregister_pio(kvm, 0x4d0);
	kvm__deregister_pio(kvm, 0x20);
	kvm__deregister_pio(kvm, 0xa0);
	device__unregister(&vpic->dev_hdr);

	kvm->arch.vpic = NULL;
	free(vpic);
}
