/*
 * x86 / x86-64 architecture-specific GDB stub support.
 *
 * GDB x86-64 register set (described in target.xml):
 *
 *  No.  Name      Size    KVM field
 *  ---  ------    ----    ---------
 *   0   rax        8      regs.rax
 *   1   rbx        8      regs.rbx
 *   2   rcx        8      regs.rcx
 *   3   rdx        8      regs.rdx
 *   4   rsi        8      regs.rsi
 *   5   rdi        8      regs.rdi
 *   6   rbp        8      regs.rbp
 *   7   rsp        8      regs.rsp
 *   8   r8         8      regs.r8
 *   9   r9         8      regs.r9
 *  10   r10        8      regs.r10
 *  11   r11        8      regs.r11
 *  12   r12        8      regs.r12
 *  13   r13        8      regs.r13
 *  14   r14        8      regs.r14
 *  15   r15        8      regs.r15
 *  16   rip        8      regs.rip
 *  17   eflags     4      regs.rflags (low 32 bits)
 *  18   cs         4      sregs.cs.selector
 *  19   ss         4      sregs.ss.selector
 *  20   ds         4      sregs.ds.selector
 *  21   es         4      sregs.es.selector
 *  22   fs         4      sregs.fs.selector
 *  23   gs         4      sregs.gs.selector
 *
 * Total: 16x8 + 8 + 4 + 6x4 = 164 bytes
 */

#include "kvm/gdb.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"

#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>

#include <linux/kvm.h>

#define GDB_NUM_REGS		24
#define GDB_REG_RIP		16
#define GDB_REG_EFLAGS		17
#define GDB_REG_CS		18

/* Byte size of the 'g' register packet */
#define GDB_REGS_SIZE		(16 * 8 + 8 + 4 + 6 * 4)	/* 164 */

#define X86_EFLAGS_TF		(1U << 8)
#define X86_EFLAGS_IF		(1U << 9)
#define X86_EFLAGS_RF		(1U << 16)

static struct {
	struct kvm_cpu *vcpu;
	bool		pending;
	bool		if_was_set;
} step_irq_state;

/* ------------------------------------------------------------------ */
/* Target XML                                                          */
/* ------------------------------------------------------------------ */

static const char target_xml[] =
	"<?xml version=\"1.0\"?>\n"
	"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
	"<target version=\"1.0\">\n"
	"  <feature name=\"org.gnu.gdb.i386.core\">\n"
	"    <reg name=\"rax\"    bitsize=\"64\"/>\n"
	"    <reg name=\"rbx\"    bitsize=\"64\"/>\n"
	"    <reg name=\"rcx\"    bitsize=\"64\"/>\n"
	"    <reg name=\"rdx\"    bitsize=\"64\"/>\n"
	"    <reg name=\"rsi\"    bitsize=\"64\"/>\n"
	"    <reg name=\"rdi\"    bitsize=\"64\"/>\n"
	"    <reg name=\"rbp\"    bitsize=\"64\"/>\n"
	"    <reg name=\"rsp\"    bitsize=\"64\"/>\n"
	"    <reg name=\"r8\"     bitsize=\"64\"/>\n"
	"    <reg name=\"r9\"     bitsize=\"64\"/>\n"
	"    <reg name=\"r10\"    bitsize=\"64\"/>\n"
	"    <reg name=\"r11\"    bitsize=\"64\"/>\n"
	"    <reg name=\"r12\"    bitsize=\"64\"/>\n"
	"    <reg name=\"r13\"    bitsize=\"64\"/>\n"
	"    <reg name=\"r14\"    bitsize=\"64\"/>\n"
	"    <reg name=\"r15\"    bitsize=\"64\"/>\n"
	"    <reg name=\"rip\"    bitsize=\"64\" type=\"code_ptr\"/>\n"
	"    <reg name=\"eflags\" bitsize=\"32\"/>\n"
	"    <reg name=\"cs\"     bitsize=\"32\" type=\"int\"/>\n"
	"    <reg name=\"ss\"     bitsize=\"32\" type=\"int\"/>\n"
	"    <reg name=\"ds\"     bitsize=\"32\" type=\"int\"/>\n"
	"    <reg name=\"es\"     bitsize=\"32\" type=\"int\"/>\n"
	"    <reg name=\"fs\"     bitsize=\"32\" type=\"int\"/>\n"
	"    <reg name=\"gs\"     bitsize=\"32\" type=\"int\"/>\n"
	"  </feature>\n"
	"</target>\n";

const char *kvm_gdb__arch_target_xml(void)
{
	return target_xml;
}

size_t kvm_gdb__arch_reg_pkt_size(void)
{
	return GDB_REGS_SIZE;
}

void kvm_gdb__arch_sync_guest_insn(void *host, size_t len)
{
}

/* ------------------------------------------------------------------ */
/* Helpers: read/write KVM register structures                        */
/* ------------------------------------------------------------------ */

static int get_regs(struct kvm_cpu *vcpu, struct kvm_regs *regs)
{
	if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, regs) < 0) {
		pr_warning("GDB: KVM_GET_REGS failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int set_regs(struct kvm_cpu *vcpu, struct kvm_regs *regs)
{
	if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, regs) < 0) {
		pr_warning("GDB: KVM_SET_REGS failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int get_sregs(struct kvm_cpu *vcpu, struct kvm_sregs *sregs)
{
	if (ioctl(vcpu->vcpu_fd, KVM_GET_SREGS, sregs) < 0) {
		pr_warning("GDB: KVM_GET_SREGS failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Register read / write                                               */
/* ------------------------------------------------------------------ */

void kvm_gdb__arch_read_registers(struct kvm_cpu *vcpu, u8 *buf, size_t *size)
{
	struct kvm_regs  regs;
	struct kvm_sregs sregs;
	u8 *p = buf;
	u32 eflags;

	*size = 0;

	if (get_regs(vcpu, &regs) < 0 || get_sregs(vcpu, &sregs) < 0)
		return;

	/* GPRs - 8 bytes each, GDB order */
#define PUT64(field) do { memcpy(p, &regs.field, 8); p += 8; } while (0)
	PUT64(rax); PUT64(rbx); PUT64(rcx); PUT64(rdx);
	PUT64(rsi); PUT64(rdi); PUT64(rbp); PUT64(rsp);
	PUT64(r8);  PUT64(r9);  PUT64(r10); PUT64(r11);
	PUT64(r12); PUT64(r13); PUT64(r14); PUT64(r15);
#undef PUT64

	/* rip (8 bytes) */
	memcpy(p, &regs.rip, 8);
	p += 8;

	/* eflags (4 bytes - low 32 bits of rflags) */
	eflags = (u32)regs.rflags;
	memcpy(p, &eflags, 4);
	p += 4;

	/* Segment selectors (4 bytes each) */
#define PUTSEL(seg) do {			\
	u32 sel = (u32)sregs.seg.selector;	\
	memcpy(p, &sel, 4);			\
	p += 4;					\
} while (0)
	PUTSEL(cs); PUTSEL(ss); PUTSEL(ds);
	PUTSEL(es); PUTSEL(fs); PUTSEL(gs);
#undef PUTSEL

	*size = (size_t)(p - buf);
}

void kvm_gdb__arch_write_registers(struct kvm_cpu *vcpu, const u8 *buf,
				    size_t size)
{
	struct kvm_regs  regs;
	struct kvm_sregs sregs;
	const u8 *p = buf;
	u32 eflags;

	if (size < GDB_REGS_SIZE)
		return;

	if (get_regs(vcpu, &regs) < 0 || get_sregs(vcpu, &sregs) < 0)
		return;

#define GET64(field) do { memcpy(&regs.field, p, 8); p += 8; } while (0)
	GET64(rax); GET64(rbx); GET64(rcx); GET64(rdx);
	GET64(rsi); GET64(rdi); GET64(rbp); GET64(rsp);
	GET64(r8);  GET64(r9);  GET64(r10); GET64(r11);
	GET64(r12); GET64(r13); GET64(r14); GET64(r15);
#undef GET64

	memcpy(&regs.rip, p, 8);
	p += 8;

	memcpy(&eflags, p, 4);
	regs.rflags = (regs.rflags & ~0xffffffffULL) | eflags;
	p += 4;

	/* Segment selectors - only update the selector field */
#define SETSEL(seg) do {				\
	u32 sel;					\
	memcpy(&sel, p, 4);				\
	sregs.seg.selector = (u16)sel;			\
	p += 4;						\
} while (0)
	SETSEL(cs); SETSEL(ss); SETSEL(ds);
	SETSEL(es); SETSEL(fs); SETSEL(gs);
#undef SETSEL

	set_regs(vcpu, &regs);
	/* We don't write sregs back for segment selector-only changes
	 * to avoid corrupting descriptor caches; GDB mainly needs rip. */
	(void)sregs;
}

int kvm_gdb__arch_read_register(struct kvm_cpu *vcpu, int regno,
				 u8 *buf, size_t *size)
{
	struct kvm_regs  regs;
	struct kvm_sregs sregs;
	struct kvm_segment *segs[6];
	u32 eflags;
	u32 sel;
	int idx;

	if (regno < 0 || regno >= GDB_NUM_REGS)
		return -1;

	if (get_regs(vcpu, &regs) < 0)
		return -1;

	if (regno >= GDB_REG_CS && get_sregs(vcpu, &sregs) < 0)
		return -1;

	if (regno < 16) {
		/* GPRs */
		static const size_t offs[] = {
			offsetof(struct kvm_regs, rax),
			offsetof(struct kvm_regs, rbx),
			offsetof(struct kvm_regs, rcx),
			offsetof(struct kvm_regs, rdx),
			offsetof(struct kvm_regs, rsi),
			offsetof(struct kvm_regs, rdi),
			offsetof(struct kvm_regs, rbp),
			offsetof(struct kvm_regs, rsp),
			offsetof(struct kvm_regs, r8),
			offsetof(struct kvm_regs, r9),
			offsetof(struct kvm_regs, r10),
			offsetof(struct kvm_regs, r11),
			offsetof(struct kvm_regs, r12),
			offsetof(struct kvm_regs, r13),
			offsetof(struct kvm_regs, r14),
			offsetof(struct kvm_regs, r15),
		};
		memcpy(buf, (u8 *)&regs + offs[regno], 8);
		*size = 8;
	} else if (regno == GDB_REG_RIP) {
		memcpy(buf, &regs.rip, 8);
		*size = 8;
	} else if (regno == GDB_REG_EFLAGS) {
		eflags = (u32)regs.rflags;
		memcpy(buf, &eflags, 4);
		*size = 4;
	} else {
		/* Segment selectors (18-23) */
		segs[0] = &sregs.cs; segs[1] = &sregs.ss; segs[2] = &sregs.ds;
		segs[3] = &sregs.es; segs[4] = &sregs.fs; segs[5] = &sregs.gs;
		idx = regno - GDB_REG_CS;
		sel = (u32)segs[idx]->selector;
		memcpy(buf, &sel, 4);
		*size = 4;
	}

	return 0;
}

int kvm_gdb__arch_write_register(struct kvm_cpu *vcpu, int regno,
				  const u8 *buf, size_t size)
{
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	struct kvm_segment *segs[6];
	u32 eflags;
	u32 sel;
	int idx;

	if (regno < 0 || regno >= GDB_NUM_REGS)
		return -1;

	if (get_regs(vcpu, &regs) < 0)
		return -1;

	if (regno < 16) {
		static const size_t offs[] = {
			offsetof(struct kvm_regs, rax),
			offsetof(struct kvm_regs, rbx),
			offsetof(struct kvm_regs, rcx),
			offsetof(struct kvm_regs, rdx),
			offsetof(struct kvm_regs, rsi),
			offsetof(struct kvm_regs, rdi),
			offsetof(struct kvm_regs, rbp),
			offsetof(struct kvm_regs, rsp),
			offsetof(struct kvm_regs, r8),
			offsetof(struct kvm_regs, r9),
			offsetof(struct kvm_regs, r10),
			offsetof(struct kvm_regs, r11),
			offsetof(struct kvm_regs, r12),
			offsetof(struct kvm_regs, r13),
			offsetof(struct kvm_regs, r14),
			offsetof(struct kvm_regs, r15),
		};
		if (size < 8) return -1;
		memcpy((u8 *)&regs + offs[regno], buf, 8);
		return set_regs(vcpu, &regs);
	}

	if (regno == GDB_REG_RIP) {
		if (size < 8) return -1;
		memcpy(&regs.rip, buf, 8);
		return set_regs(vcpu, &regs);
	}

	if (regno == GDB_REG_EFLAGS) {
		if (size < 4) return -1;
		memcpy(&eflags, buf, 4);
		regs.rflags = (regs.rflags & ~0xffffffffULL) | eflags;
		return set_regs(vcpu, &regs);
	}

	/* Segment selector: write via sregs */
	if (get_sregs(vcpu, &sregs) < 0)
		return -1;

	segs[0] = &sregs.cs; segs[1] = &sregs.ss; segs[2] = &sregs.ds;
	segs[3] = &sregs.es; segs[4] = &sregs.fs; segs[5] = &sregs.gs;
	idx = regno - GDB_REG_CS;
	if (size < 4) return -1;
	memcpy(&sel, buf, 4);
	segs[idx]->selector = (u16)sel;

	if (ioctl(vcpu->vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
		return -1;

	return 0;
}

/* ------------------------------------------------------------------ */
/* PC                                                                  */
/* ------------------------------------------------------------------ */

u64 kvm_gdb__arch_get_pc(struct kvm_cpu *vcpu)
{
	struct kvm_regs regs;
	if (get_regs(vcpu, &regs) < 0)
		return 0;
	return regs.rip;
}

void kvm_gdb__arch_set_pc(struct kvm_cpu *vcpu, u64 pc)
{
	struct kvm_regs regs;
	if (get_regs(vcpu, &regs) < 0)
		return;
	regs.rip = pc;
	set_regs(vcpu, &regs);
}

/* ------------------------------------------------------------------ */
/* Debug control (single-step + hardware breakpoints)                 */
/* ------------------------------------------------------------------ */

/*
 * DR7 bit layout:
 *   G0..G3 (bits 1,3,5,7): global enable for DR0..DR3
 *   cond0..cond3 (bits 16-17, 20-21, 24-25, 28-29):
 *     00=execution, 01=write, 11=read/write
 *   len0..len3 (bits 18-19, 22-23, 26-27, 30-31):
 *     00=1B, 01=2B, 10=8B, 11=4B
 */

static u64 dr7_for_bp(struct kvm_gdb_hw_bp *bps)
{
	u64 dr7 = 0;
	u64 cond;
	u64 len;
	int i;

	for (i = 0; i < 4; i++) {
		if (!bps[i].active)
			continue;

		/* Global enable bit */
		dr7 |= (1ULL << (i * 2 + 1));

		/* Condition */
		switch (bps[i].type) {
		case 0:  cond = 0; break;	/* execution  (00) */
		case 1:  cond = 1; break;	/* write      (01) */
		case 2:  cond = 3; break;	/* read/write (11) - no read-only */
		case 3:  cond = 3; break;	/* access     (11) */
		default: cond = 0; break;
		}
		dr7 |= (cond << (16 + i * 4));

		/* Length */
		switch (bps[i].len) {
		case 1:  len = 0; break;	/* 1B (00) */
		case 2:  len = 1; break;	/* 2B (01) */
		case 4:  len = 3; break;	/* 4B (11) */
		case 8:  len = 2; break;	/* 8B (10) */
		default: len = 0; break;
		}
		dr7 |= (len << (18 + i * 4));
	}

	return dr7;
}

void kvm_gdb__arch_set_debug(struct kvm_cpu *vcpu, bool single_step,
			      struct kvm_gdb_hw_bp *hw_bps)
{
	struct kvm_guest_debug dbg = { 0 };
	u64 dr7;
	int i;

	dbg.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP;

	if (single_step)
		dbg.control |= KVM_GUESTDBG_SINGLESTEP;

	if (hw_bps) {
		dr7 = dr7_for_bp(hw_bps);
		if (dr7) {
			dbg.control |= KVM_GUESTDBG_USE_HW_BP;
			for (i = 0; i < 4; i++) {
				if (hw_bps[i].active)
					dbg.arch.debugreg[i] = hw_bps[i].addr;
			}
			dbg.arch.debugreg[7] = dr7;
		}
	}

	if (ioctl(vcpu->vcpu_fd, KVM_SET_GUEST_DEBUG, &dbg) < 0)
		pr_warning("GDB: KVM_SET_GUEST_DEBUG failed: %s",
			   strerror(errno));
}

void kvm_gdb__arch_prepare_resume(struct kvm_cpu *vcpu, bool single_step,
				   bool from_debug_exit)
{
	struct kvm_regs regs;

	if (!from_debug_exit)
		return;

	if (get_regs(vcpu, &regs) < 0)
		return;

	regs.rflags &= ~X86_EFLAGS_TF;
	if (single_step)
		regs.rflags |= X86_EFLAGS_TF;

	if (single_step) {
		step_irq_state.vcpu = vcpu;
		step_irq_state.pending = true;
		step_irq_state.if_was_set = !!(regs.rflags & X86_EFLAGS_IF);
		regs.rflags &= ~X86_EFLAGS_IF;
	}

	regs.rflags |= X86_EFLAGS_RF;
	set_regs(vcpu, &regs);
}

void kvm_gdb__arch_handle_stop(struct kvm_cpu *vcpu)
{
	struct kvm_regs regs;

	if (!step_irq_state.pending || step_irq_state.vcpu != vcpu)
		return;

	if (get_regs(vcpu, &regs) < 0)
		return;

	if (step_irq_state.if_was_set)
		regs.rflags |= X86_EFLAGS_IF;
	else
		regs.rflags &= ~X86_EFLAGS_IF;

	set_regs(vcpu, &regs);
	step_irq_state.pending = false;
	step_irq_state.vcpu = NULL;
}

/* ------------------------------------------------------------------ */
/* Stop signal                                                         */
/* ------------------------------------------------------------------ */

int kvm_gdb__arch_signal(struct kvm_cpu *vcpu)
{
	/* Always report SIGTRAP (5) */
	return 5;
}

/* ------------------------------------------------------------------ */
/* Software-breakpoint re-injection                                    */
/* ------------------------------------------------------------------ */

/*
 * x86 exception numbers in kvm_run->debug.arch.exception:
 *   1  = #DB  (single-step / hardware breakpoint)
 *   3  = #BP  (INT3 software breakpoint)
 */
bool kvm_gdb__arch_is_sw_bp_exit(struct kvm_cpu *vcpu)
{
	return vcpu->kvm_run->debug.arch.exception == 3;
}

/*
 * Return the address of the INT3 byte that triggered the exit.
 *
 * KVM intercepts the #BP VM-exit BEFORE delivering the exception to the
 * guest.  At that point the guest RIP still points at the INT3 instruction
 * itself (not the next byte), and KVM copies that value into
 * kvm_run->debug.arch.pc.  So no adjustment is needed.
 *
 * (Earlier code subtracted 1 here, which was wrong: it produced an address
 * one byte before the INT3, causing sw_bp_active_at() to miss every hit.)
 */
u64 kvm_gdb__arch_debug_pc(struct kvm_cpu *vcpu)
{
	return vcpu->kvm_run->debug.arch.pc;
}

/*
 * Re-inject the #BP exception so the guest's own INT3 handler sees it.
 *
 * At this point:
 *   - Guest RIP points at the INT3 byte itself (KVM intercepted the VM-exit
 *     before the exception was delivered, so the CPU has not yet advanced RIP).
 *   - We inject exception #3 with no error code.
 *   - When KVM delivers the injected #BP, the CPU will advance RIP past the
 *     INT3 and push RIP+1 into the exception frame, which is the standard
 *     x86 #BP convention the guest's handler expects.
 */
void kvm_gdb__arch_reinject_sw_bp(struct kvm_cpu *vcpu)
{
	struct kvm_vcpu_events events;

	if (ioctl(vcpu->vcpu_fd, KVM_GET_VCPU_EVENTS, &events) < 0) {
		pr_warning("GDB: KVM_GET_VCPU_EVENTS failed: %s",
			   strerror(errno));
		return;
	}

	events.exception.injected     = 1;
	events.exception.nr           = 3;	/* #BP */
	events.exception.has_error_code = 0;

	if (ioctl(vcpu->vcpu_fd, KVM_SET_VCPU_EVENTS, &events) < 0)
		pr_warning("GDB: KVM_SET_VCPU_EVENTS failed: %s",
			   strerror(errno));
}
