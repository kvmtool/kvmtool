#include "kvm/kvm.h"
#include "kvm/i8254.h"
#include "kvm/timer.h"
#include "kvm/irq.h"

#define RW_STATE_LSB 1
#define RW_STATE_MSB 2
#define RW_STATE_WORD0 3
#define RW_STATE_WORD1 4

static void pit_irq_timer_update(struct kvm_kpit_channel_state *s, s64 current_time);
static void pit_irq_timer(union sigval sv);

static s64 cpu_get_clock(struct kvm_kpit_channel_state *s)
{
	s64 time;
	time = s->cpu_clock_offset;
	if (s->cpu_ticks_enabled) {
		time += get_clock();
	}

	return time;
}

static int start_timer(struct kvm_kpit_channel_state *t)
{
	struct sigevent ev;
	timer_t host_timer;

	/* 
	 * Initialize ev struct to 0 to avoid valgrind complaining
	 * about uninitialized data in timer_create call
	 */
	memset(&ev, 0, sizeof(ev));
	ev.sigev_value.sival_ptr = t;
	ev.sigev_notify = SIGEV_THREAD;
	ev.sigev_notify_function = pit_irq_timer;

	if (timer_create(CLOCK_REALTIME, &ev, &host_timer)) {
		perror("timer_create");
		return -1;
	}

	t->timer = host_timer;

	return 0;
}

static void stop_timer(struct kvm_kpit_channel_state *t)
{
	timer_t host_timer = t->timer;
	struct itimerspec timeout;
	timeout.it_interval.tv_sec = 0;
	timeout.it_interval.tv_nsec = 0;
	timeout.it_value.tv_sec =  0;
	timeout.it_value.tv_nsec = 0;
	if (timer_settime(host_timer, 0 /* RELATIVE */, &timeout, NULL)) {
		perror("settime");
		fprintf(stderr, "Internal timer error: aborting\n");
		exit(1);
	}
}

static void destroy_timer(struct kvm_kpit_channel_state *t)
{
	timer_t host_timer = t->timer;
	timer_delete(host_timer);
}

static void rearm_timer(struct kvm_kpit_channel_state *t,
								 s64 expire_time)
{
	s64 nearest_delta_ns = expire_time - cpu_get_clock(t);
	timer_t host_timer = t->timer;
	struct itimerspec timeout;
	s64 current_ns;

	if (nearest_delta_ns < MIN_TIMER_REARM_NS)
		nearest_delta_ns = MIN_TIMER_REARM_NS;
	
	/* check whether a timer is already running */
	if (timer_gettime(host_timer, &timeout)) {
		perror("gettime");
		fprintf(stderr, "Internal timer error: aborting\n");
		exit(1);
	}
	current_ns = timeout.it_value.tv_sec * 1000000000LL + timeout.it_value.tv_nsec;
	if (current_ns && current_ns <= nearest_delta_ns)
		return;

	timeout.it_interval.tv_sec = 0;
	timeout.it_interval.tv_nsec = 0; /* 0 for one-shot timer */
	timeout.it_value.tv_sec =  nearest_delta_ns / 1000000000;
	timeout.it_value.tv_nsec = nearest_delta_ns % 1000000000;
	if (timer_settime(host_timer, 0 /* RELATIVE */, &timeout, NULL)) {
		perror("settime");
		fprintf(stderr, "Internal timer error: aborting\n");
		exit(1);
	}
}

void clock_enable(struct kvm *kvm, int enabled)
{
	struct kvm_pit *pit = kvm->arch.pit;
	struct kvm_kpit_channel_state *s = &pit->pit_state.channels[0];
	s->cpu_ticks_enabled = enabled;
	struct itimerspec timeout;

	if (enabled) {
		rearm_timer(s, s->remaining_time);
	} else {
		/* check whether a timer is already running */
		if (timer_gettime(s->timer, &timeout)) {
			perror("gettime");
			fprintf(stderr, "Internal timer error: aborting\n");
			exit(1);
		}
		s->remaining_time = timeout.it_value.tv_sec * 1000000000LL + timeout.it_value.tv_nsec;
		stop_timer(s);
	}
}

/*
 * enable cpu_get_ticks()
 */
void cpu_enable_ticks(struct kvm *kvm)
{
	// mutex_lock(&s->vm_clock_lock);
	struct kvm_pit *pit = kvm->arch.pit;
	struct kvm_kpit_channel_state *s = &pit->pit_state.channels[0];
	if (!s->cpu_ticks_enabled) {
		s->cpu_clock_offset -= get_clock();
		s->cpu_ticks_enabled = 1;
	}
	// mutex_unlock(&s->vm_clock_lock);
}

/*
 * disable cpu_get_ticks() : the clock is stopped. You must not call
 * cpu_get_ticks() after that.
 */
void cpu_disable_ticks(struct kvm *kvm)
{
	// mutex_lock(&s->vm_clock_lock);
	struct kvm_pit *pit = kvm->arch.pit;
	struct kvm_kpit_channel_state *s = &pit->pit_state.channels[0];
	if (s->cpu_ticks_enabled) {
		s->cpu_clock_offset = cpu_get_clock(s);
		s->cpu_ticks_enabled = 0;
	}
	// mutex_unlock(&s->vm_clock_lock);
}

// static void pit_set_gate(struct kvm_pit *pit, int channel, u32 val)
// {
// 	struct kvm_kpit_channel_state *c = &pit->pit_state.channels[channel];

// 	switch (c->mode) {
// 	default:
// 	case 0:
// 	case 4:
// 		/* XXX: just disable/enable counting */
// 		break;
// 	case 1:
// 	case 2:
// 	case 3:
// 	case 5:
// 		/* Restart counting on rising edge. */
// 		if (c->gate < val){
// 			c->count_load_time = cpu_get_clock(c);
// 			pit_irq_timer_update(c, c->count_load_time);
// 		}
// 		break;
// 	}

// 	c->gate = val;
// }

// static int pit_get_gate(struct kvm_pit *pit, int channel)
// {
// 	return pit->pit_state.channels[channel].gate;
// }

static int pit_get_count(struct kvm_pit *pit, int channel)
{
	struct kvm_kpit_channel_state *c = &pit->pit_state.channels[channel];
	s64 d;
	int counter;

	d = muldiv64(cpu_get_clock(c) - c->count_load_time, PIT_FREQ,
					NANOSECONDS_PER_SECOND);

	switch (c->mode) {
	case 0:
	case 1:
	case 4:
	case 5:
		counter = (c->count - d) & 0xffff;
		break;
	case 3:
		/* XXX: may be incorrect for odd counts */
		counter = c->count - ((2 * d) % c->count);
		break;
	default:
		counter = c->count - (d % c->count);
		break;
	}
	return counter;
}

static int pit_get_out(struct kvm_kpit_channel_state *c, s64 current_time)
{
	s64 d;
	int out;

	d = muldiv64(current_time - c->count_load_time, PIT_FREQ,
				NANOSECONDS_PER_SECOND);

	switch (c->mode) {
	default:
	case 0:
		out = (d >= c->count);
		break;
	case 1:
		out = (d < c->count);
		break;
	case 2:
		out = (((d % c->count) == 0) && (d != 0));
		break;
	case 3:
		out = ((d % c->count) < ((c->count + 1) >> 1));
		break;
	case 4:
	case 5:
		out = (d == c->count);
		break;
	}

	return out;
}

static void pit_latch_count(struct kvm_pit *pit, int channel)
{
	struct kvm_kpit_channel_state *c = &pit->pit_state.channels[channel];

	if (!c->count_latched) {
		c->latched_count = pit_get_count(pit, channel);
		c->count_latched = c->rw_mode;
	}
}

static void pit_latch_status(struct kvm_pit *pit, int channel)
{
	struct kvm_kpit_channel_state *c = &pit->pit_state.channels[channel];

	if (!c->status_latched) {
		/* TODO: Return NULL COUNT (bit 6). */
		c->status = ((pit_get_out(c, cpu_get_clock(c)) << 7) |
				(c->rw_mode << 4) |
				(c->mode << 1) |
				c->bcd);
		c->status_latched = 1;
	}
}

static inline struct kvm_pit *pit_state_to_pit(struct kvm_kpit_state *ps)
{
	return container_of(ps, struct kvm_pit, pit_state);
}

static void pit_load_count(struct kvm_pit *pit, int channel, u32 val)
{
	struct kvm_kpit_state *ps = &pit->pit_state;

	/*
	 * The largest possible initial count is 0; this is equivalent
	 * to 216 for binary counting and 104 for BCD counting.
	 */
	if (val == 0)
		val = 0x10000;
	struct kvm_kpit_channel_state *c = &ps->channels[channel];
	c->count = val;
	c->count_load_time = cpu_get_clock(c);
	pit_irq_timer_update(c, c->count_load_time);
}

static int pit_ioport_write(struct kvm_pit *pit, gpa_t addr, u32 len, const void *data)
{
	u8 val = *(u8 *)data;
	struct kvm_kpit_state *pit_state = &pit->pit_state;
	int channel, access;
	struct kvm_kpit_channel_state *s;

	mutex_lock(&pit_state->lock);

	addr &= KVM_PIT_CHANNEL_MASK;
	if (addr == 3) {
		channel = val >> 6;
		if (channel == 3) {
			/* Read-Back Command. */
			for (channel = 0; channel < 3; channel++) {
				if (val & (2 << channel)) {
					if (!(val & 0x20))
						pit_latch_count(pit, channel);
					if (!(val & 0x10))
						pit_latch_status(pit, channel);
				}
			}
		} else {
			/* Select Counter <channel>. */
			s = &pit_state->channels[channel];
			access = (val >> 4) & KVM_PIT_CHANNEL_MASK;
			if (access == 0) {
				pit_latch_count(pit, channel);
			} else {
				s->rw_mode = access;
				s->read_state = access;
				s->write_state = access;
				s->mode = (val >> 1) & 7;
				if (s->mode > 5)
					s->mode -= 4;
				s->bcd = val & 1;
			}
		}
	} else {
		/* Write Count. */
		s = &pit_state->channels[addr];
		switch (s->write_state) {
		default:
		case RW_STATE_LSB:
			pit_load_count(pit, addr, val);
			break;
		case RW_STATE_MSB:
			pit_load_count(pit, addr, val << 8);
			break;
		case RW_STATE_WORD0:
			s->write_latch = val;
			s->write_state = RW_STATE_WORD1;
			break;
		case RW_STATE_WORD1:
			pit_load_count(pit, addr, s->write_latch | (val << 8));
			s->write_state = RW_STATE_WORD0;
			break;
		}
	}

	mutex_unlock(&pit_state->lock);
	return 0;
}

static int pit_ioport_read(struct kvm_pit *pit, gpa_t addr, u32 len, void *data)
{
	struct kvm_kpit_state *pit_state = &pit->pit_state;
	int ret, count;
	struct kvm_kpit_channel_state *s;

	addr &= KVM_PIT_CHANNEL_MASK;
	if (addr == 3)
		return 0;

	s = &pit_state->channels[addr];

	mutex_lock(&pit_state->lock);

	if (s->status_latched) {
		s->status_latched = 0;
		ret = s->status;
	} else if (s->count_latched) {
		switch (s->count_latched) {
		default:
		case RW_STATE_LSB:
			ret = s->latched_count & 0xff;
			s->count_latched = 0;
			break;
		case RW_STATE_MSB:
			ret = s->latched_count >> 8;
			s->count_latched = 0;
			break;
		case RW_STATE_WORD0:
			ret = s->latched_count & 0xff;
			s->count_latched = RW_STATE_MSB;
			break;
		}
	} else {
		switch (s->read_state) {
		default:
		case RW_STATE_LSB:
			count = pit_get_count(pit, addr);
			ret = count & 0xff;
			break;
		case RW_STATE_MSB:
			count = pit_get_count(pit, addr);
			ret = (count >> 8) & 0xff;
			break;
		case RW_STATE_WORD0:
			count = pit_get_count(pit, addr);
			ret = count & 0xff;
			s->read_state = RW_STATE_WORD1;
			break;
		case RW_STATE_WORD1:
			count = pit_get_count(pit, addr);
			ret = (count >> 8) & 0xff;
			s->read_state = RW_STATE_WORD0;
			break;
		}
	}

	if (len > sizeof(ret))
		len = sizeof(ret);
	memcpy(data, (char *)&ret, len);

	mutex_unlock(&pit_state->lock);
	return 0;
}

static void pit_io_handler(struct kvm_cpu *vpcu, u64 addr, u8 *data,
				u32 len, u8 is_write, void *ptr)
{
	if (is_write)
		pit_ioport_write(ptr, addr, len, data);
	else
		pit_ioport_read(ptr, addr, len, data);
}

/* return -1 if no transition will occur.  */
static s64 pit_get_next_transition_time(struct kvm_kpit_channel_state *s, s64 current_time)
{
	s64 d, next_time, base;
	u32 period2;

	d = muldiv64(current_time - s->count_load_time, PIT_FREQ,
				 NANOSECONDS_PER_SECOND);
	switch (s->mode) {
	default:
	case 0:
	case 1:
		if (d < s->count) {
			next_time = s->count;
		} else {
			return -1;
		}
		break;
	case 2:
		base = ALIGN_DOWN(d, s->count);
		if ((d - base) == 0 && d != 0) {
			next_time = base + s->count;
		} else {
			next_time = base + s->count + 1;
		}
		break;
	case 3:
		base = ALIGN_DOWN(d, s->count);
		period2 = ((s->count + 1) >> 1);
		if ((d - base) < period2) {
			next_time = base + period2;
		} else {
			next_time = base + s->count;
		}
		break;
	case 4:
	case 5:
		if (d < s->count) {
			next_time = s->count;
		} else if (d == s->count) {
			next_time = s->count + 1;
		} else {
			return -1;
		}
		break;
	}
	/* convert to timer units */
	next_time = s->count_load_time + muldiv64(next_time, NANOSECONDS_PER_SECOND,
											  PIT_FREQ);
	/* fix potential rounding problems */
	/* XXX: better solution: use a clock at PIT_FREQ Hz */
	if (next_time <= current_time) {
		next_time = current_time + 1;
	}
	return next_time;
}

static void pit_irq_timer_update(struct kvm_kpit_channel_state *s, s64 current_time)
{
	s64 expire_time;
	int irq_level;

	if (!s->timer || s->irq_disabled) {
		return;
	}
	expire_time = pit_get_next_transition_time(s, current_time);
	irq_level = pit_get_out(s, current_time);
	kvm__irq_line(s->pit->kvm, s->pit->irq_source_id, irq_level);
#ifdef DEBUG_PIT
	printf("irq_level=%d next_delay=%f\n",
		   irq_level,
		   (double)(expire_time - current_time) / NANOSECONDS_PER_SECOND);
#endif
	s->next_transition_time = expire_time;
	if (expire_time != -1)
		rearm_timer(s, expire_time);
	else
		stop_timer(s);
}

// static void pit_reset(struct kvm_pit *pit)
// {
// 	int i;
// 	struct kvm_kpit_channel_state *c;

// 	for (i = 0; i < 3; i++) {
// 		c = &pit->pit_state.channels[i];
// 		c->mode = 0xff;
// 		c->gate = (i != 2);
// 		pit_load_count(pit, i, 0);
// 	}
// }

static void pit_irq_timer(union sigval sv)
{
	struct kvm_kpit_channel_state *s = (struct kvm_kpit_channel_state *)sv.sival_ptr;
	pit_irq_timer_update(s, s->next_transition_time);
}

int pit_init(struct kvm *kvm) {
	struct kvm_pit *pit;
	struct kvm_kpit_state *pit_state;
	int ret;
	int i;

	pit = malloc(sizeof(struct kvm_pit));
	if (!pit)
		return -ENOMEM;
	memset(pit, 0, sizeof(struct kvm_pit));

	pit->irq_source_id = 0;
	if (pit->irq_source_id < 0) {
		ret = pit->irq_source_id;
		goto fail_request;
	}

	mutex_init(&pit->pit_state.lock);

	pit->kvm = kvm;

	pit_state = &pit->pit_state;

	for (i = 0; i < 3; i++) {
		pit_state->channels[i].pit = pit;
	}
	/* the timer 0 is connected to an IRQ */
	start_timer(&pit_state->channels[0]);
	// pit_reset(pit);


	pit->dev_hdr.bus_type = DEVICE_BUS_IOPORT;
	pit->dev_hdr.data = pit;
	ret = device__register(&pit->dev_hdr);
	if (ret < 0)
		goto fail_device;
	ret = kvm__register_pio(kvm, KVM_PIT_BASE_ADDRESS, KVM_PIT_MEM_LENGTH, pit_io_handler, pit);

	if (ret < 0)
		goto fail_reg;

	kvm->arch.pit = pit;
	cpu_enable_ticks(kvm);
	return 0;

fail_reg:
	device__unregister(&pit->dev_hdr);
fail_device:
	destroy_timer(&pit_state->channels[0]);
fail_request:
	free(pit);
	return ret;
}

void pit_destroy(struct kvm *kvm)
{
	struct kvm_pit *pit = kvm->arch.pit;

	if (!pit)
		return;

	kvm__deregister_pio(kvm, KVM_PIT_BASE_ADDRESS);
	device__unregister(&pit->dev_hdr);
	destroy_timer(&pit->pit_state.channels[0]);

	kvm->arch.pit = NULL;
	free(pit);
}
