/*
 * AArch64 architecture-specific GDB stub support.
 *
 * GDB AArch64 register set (org.gnu.gdb.aarch64.core, described in target.xml):
 *
 *  No.  Name    Size   KVM field
 *  ---  ------  ----   ---------
 *   0   x0       8     regs.regs[0]
 *   1   x1       8     regs.regs[1]
 *   ...
 *  30   x30      8     regs.regs[30]  (link register)
 *  31   sp       8     sp_el1  (kernel SP; SP_EL0 when PSTATE.EL==0)
 *  32   pc       8     regs.pc
 *  33   cpsr     4     regs.pstate (low 32 bits)
 *
 * Total: 31x8 + 8 + 8 + 4 = 268 bytes
 *
 * Software breakpoints:
 *   BRK #0  ->  little-endian bytes: 0x00 0x00 0x20 0xD4
 *   (u32 = 0xD4200000)
 *   ARM64 BRK is always 4 bytes and must be 4-byte aligned.
 *
 * Debug exit detection via ESR_EL2 (kvm_run->debug.arch.hsr):
 *   EC = bits[31:26]
 *   0x3C = BRK64  (AArch64 BRK instruction)  -> software breakpoint
 *   0x32 = SSTEP  (software single-step)
 *   0x30 = HW_BP  (hardware execution breakpoint)
 *   0x35 = WPTFAR (watchpoint)
 */

#include "kvm/gdb.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"

#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>

#include <asm/ptrace.h>
#include <linux/kvm.h>

/* ------------------------------------------------------------------ */
/* Register layout constants                                           */
/* ------------------------------------------------------------------ */

#define GDB_NUM_REGS		34	/* x0-x30, sp, pc, cpsr */
#define GDB_REG_SP		31
#define GDB_REG_PC		32
#define GDB_REG_CPSR		33

/* Byte size of the 'g' register packet: 31x8 + 8 + 8 + 4 = 268 */
#define GDB_REGS_SIZE		268

/* ESR EC field */
#define ESR_EC_SHIFT		26
#define ESR_EC_BRK64		0x3C	/* AArch64 BRK instruction */

#define ARM64_DAIF_MASK		(PSR_A_BIT | PSR_I_BIT | PSR_F_BIT)

static struct {
	struct kvm_cpu *vcpu;
	u32		daif_bits;
	bool		pending;
} step_irq_state;

/* ------------------------------------------------------------------ */
/* ARM64_CORE_REG helper (same logic as arm/aarch64/kvm-cpu.c)        */
/* ------------------------------------------------------------------ */

static __u64 __core_reg_id(__u64 offset)
{
	__u64 id = KVM_REG_ARM64 | KVM_REG_ARM_CORE | offset;

	if (offset < KVM_REG_ARM_CORE_REG(fp_regs))
		id |= KVM_REG_SIZE_U64;
	else if (offset < KVM_REG_ARM_CORE_REG(fp_regs.fpsr))
		id |= KVM_REG_SIZE_U128;
	else
		id |= KVM_REG_SIZE_U32;

	return id;
}

#define ARM64_CORE_REG(x)	__core_reg_id(KVM_REG_ARM_CORE_REG(x))

/* VBAR_EL1: S3_0_C12_C0_0  (op0=3, op1=0, CRn=12, CRm=0, op2=0) */
#define KVM_REG_VBAR_EL1	ARM64_SYS_REG(3, 0, 12, 0, 0)
/* ESR_EL1:  S3_0_C5_C2_0   (op0=3, op1=0, CRn=5, CRm=2, op2=0) */
#define KVM_REG_ESR_EL1		ARM64_SYS_REG(3, 0, 5, 2, 0)

/* ------------------------------------------------------------------ */
/* Single-register get/set helpers                                     */
/* ------------------------------------------------------------------ */

static int get_one_reg(struct kvm_cpu *vcpu, __u64 id, u64 *val)
{
	struct kvm_one_reg reg = { .id = id, .addr = (u64)val };

	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0) {
		pr_warning("GDB: KVM_GET_ONE_REG id=0x%llx failed: %s",
			   (unsigned long long)id, strerror(errno));
		return -1;
	}
	return 0;
}

static int set_one_reg(struct kvm_cpu *vcpu, __u64 id, u64 val)
{
	struct kvm_one_reg reg = { .id = id, .addr = (u64)&val };

	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0) {
		pr_warning("GDB: KVM_SET_ONE_REG id=0x%llx failed: %s",
			   (unsigned long long)id, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * pstate for KVM_GET_ONE_REG is 32-bit; wrap it so the 64-bit helper works.
 */
static int get_pstate(struct kvm_cpu *vcpu, u32 *out)
{
	u64 id = ARM64_CORE_REG(regs.pstate);
	u32 val;
	struct kvm_one_reg reg = { .id = id, .addr = (u64)&val };

	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0) {
		pr_warning("GDB: KVM_GET_ONE_REG(pstate) failed: %s",
			   strerror(errno));
		return -1;
	}
	*out = val;
	return 0;
}

static int set_pstate(struct kvm_cpu *vcpu, u32 val)
{
	u64 id = ARM64_CORE_REG(regs.pstate);
	struct kvm_one_reg reg = { .id = id, .addr = (u64)&val };

	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0) {
		pr_warning("GDB: KVM_SET_ONE_REG(pstate) failed: %s",
			   strerror(errno));
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Target XML                                                          */
/* ------------------------------------------------------------------ */

static const char target_xml[] =
	"<?xml version=\"1.0\"?>\n"
	"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
	"<target version=\"1.0\">\n"
	"  <feature name=\"org.gnu.gdb.aarch64.core\">\n"
	"    <reg name=\"x0\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x1\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x2\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x3\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x4\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x5\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x6\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x7\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x8\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x9\"   bitsize=\"64\"/>\n"
	"    <reg name=\"x10\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x11\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x12\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x13\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x14\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x15\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x16\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x17\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x18\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x19\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x20\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x21\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x22\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x23\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x24\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x25\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x26\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x27\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x28\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x29\"  bitsize=\"64\"/>\n"
	"    <reg name=\"x30\"  bitsize=\"64\"/>\n"
	"    <reg name=\"sp\"   bitsize=\"64\" type=\"data_ptr\"/>\n"
	"    <reg name=\"pc\"   bitsize=\"64\" type=\"code_ptr\"/>\n"
	"    <reg name=\"cpsr\" bitsize=\"32\"/>\n"
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
	char *start = host;
	char *end = start + len;

	__builtin___clear_cache(start, end);
}

/* ------------------------------------------------------------------ */
/* Helpers: which SP to expose as GDB register 31                     */
/* ------------------------------------------------------------------ */

/*
 * When the guest is in EL1 (kernel mode), the active stack pointer is SP_EL1.
 * When in EL0 (user mode), the active SP is SP_EL0 (regs.sp in kvm_regs).
 * Return the appropriate KVM register ID for the active SP.
 */
static __u64 sp_reg_id(struct kvm_cpu *vcpu)
{
	u32 pstate;

	if (get_pstate(vcpu, &pstate) < 0)
		return ARM64_CORE_REG(sp_el1);	/* best-effort default */

	/* PSTATE.EL = bits [3:2] */
	if (((pstate >> 2) & 0x3) >= 1)
		return ARM64_CORE_REG(sp_el1);
	else
		return ARM64_CORE_REG(regs.sp);
}

/* ------------------------------------------------------------------ */
/* Register read / write (bulk 'g'/'G' packet)                        */
/* ------------------------------------------------------------------ */

void kvm_gdb__arch_read_registers(struct kvm_cpu *vcpu, u8 *buf, size_t *size)
{
	u8 *p = buf;
	u32 pstate;
	int i;

	*size = 0;

	/* x0-x30: 31 x 8 bytes */
	for (i = 0; i < 31; i++) {
		u64 xn;

		if (get_one_reg(vcpu, ARM64_CORE_REG(regs.regs[i]), &xn) < 0)
			return;
		memcpy(p, &xn, 8);
		p += 8;
	}

	/* sp (register 31): 8 bytes -- active stack pointer */
	{
		u64 sp;

		if (get_one_reg(vcpu, sp_reg_id(vcpu), &sp) < 0)
			return;
		memcpy(p, &sp, 8);
		p += 8;
	}

	/* pc (register 32): 8 bytes */
	{
		u64 pc;

		if (get_one_reg(vcpu, ARM64_CORE_REG(regs.pc), &pc) < 0)
			return;
		memcpy(p, &pc, 8);
		p += 8;
	}

	/* cpsr (register 33): 4 bytes -- low 32 bits of pstate */
	if (get_pstate(vcpu, &pstate) < 0)
		return;
	memcpy(p, &pstate, 4);
	p += 4;

	*size = (size_t)(p - buf);
}

void kvm_gdb__arch_write_registers(struct kvm_cpu *vcpu, const u8 *buf,
				    size_t size)
{
	const u8 *p = buf;
	int i;

	if (size < GDB_REGS_SIZE)
		return;

	/* x0-x30 */
	for (i = 0; i < 31; i++) {
		u64 xn;

		memcpy(&xn, p, 8);
		p += 8;
		if (set_one_reg(vcpu, ARM64_CORE_REG(regs.regs[i]), xn) < 0)
			return;
	}

	/* sp */
	{
		u64 sp;

		memcpy(&sp, p, 8);
		p += 8;
		if (set_one_reg(vcpu, sp_reg_id(vcpu), sp) < 0)
			return;
	}

	/* pc */
	{
		u64 pc;

		memcpy(&pc, p, 8);
		p += 8;
		if (set_one_reg(vcpu, ARM64_CORE_REG(regs.pc), pc) < 0)
			return;
	}

	/* cpsr */
	{
		u32 pstate;

		memcpy(&pstate, p, 4);
		p += 4;
		set_pstate(vcpu, pstate);
	}
}

/* ------------------------------------------------------------------ */
/* Single-register read/write ('p n' / 'P n=v')                       */
/* ------------------------------------------------------------------ */

int kvm_gdb__arch_read_register(struct kvm_cpu *vcpu, int regno,
				 u8 *buf, size_t *size)
{
	if (regno < 0 || regno >= GDB_NUM_REGS)
		return -1;

	if (regno < 31) {
		/* x0 - x30 */
		u64 xn;

		if (get_one_reg(vcpu, ARM64_CORE_REG(regs.regs[regno]), &xn) < 0)
			return -1;
		memcpy(buf, &xn, 8);
		*size = 8;
	} else if (regno == GDB_REG_SP) {
		u64 sp;

		if (get_one_reg(vcpu, sp_reg_id(vcpu), &sp) < 0)
			return -1;
		memcpy(buf, &sp, 8);
		*size = 8;
	} else if (regno == GDB_REG_PC) {
		u64 pc;

		if (get_one_reg(vcpu, ARM64_CORE_REG(regs.pc), &pc) < 0)
			return -1;
		memcpy(buf, &pc, 8);
		*size = 8;
	} else {
		/* GDB_REG_CPSR */
		u32 pstate;

		if (get_pstate(vcpu, &pstate) < 0)
			return -1;
		memcpy(buf, &pstate, 4);
		*size = 4;
	}

	return 0;
}

int kvm_gdb__arch_write_register(struct kvm_cpu *vcpu, int regno,
				  const u8 *buf, size_t size)
{
	if (regno < 0 || regno >= GDB_NUM_REGS)
		return -1;

	if (regno < 31) {
		u64 xn;

		if (size < 8)
			return -1;
		memcpy(&xn, buf, 8);
		return set_one_reg(vcpu, ARM64_CORE_REG(regs.regs[regno]), xn);
	} else if (regno == GDB_REG_SP) {
		u64 sp;

		if (size < 8)
			return -1;
		memcpy(&sp, buf, 8);
		return set_one_reg(vcpu, sp_reg_id(vcpu), sp);
	} else if (regno == GDB_REG_PC) {
		u64 pc;

		if (size < 8)
			return -1;
		memcpy(&pc, buf, 8);
		return set_one_reg(vcpu, ARM64_CORE_REG(regs.pc), pc);
	} else {
		/* GDB_REG_CPSR */
		u32 pstate;

		if (size < 4)
			return -1;
		memcpy(&pstate, buf, 4);
		return set_pstate(vcpu, pstate);
	}
}

/* ------------------------------------------------------------------ */
/* PC                                                                  */
/* ------------------------------------------------------------------ */

u64 kvm_gdb__arch_get_pc(struct kvm_cpu *vcpu)
{
	u64 pc = 0;

	get_one_reg(vcpu, ARM64_CORE_REG(regs.pc), &pc);
	return pc;
}

void kvm_gdb__arch_set_pc(struct kvm_cpu *vcpu, u64 pc)
{
	set_one_reg(vcpu, ARM64_CORE_REG(regs.pc), pc);
}

/* ------------------------------------------------------------------ */
/* Debug control (single-step + hardware breakpoints / watchpoints)   */
/* ------------------------------------------------------------------ */

/*
 * BCR (Breakpoint Control Register) for an enabled execution breakpoint:
 *
 *   Bit  1    : EN = 1 (enable)
 *   Bits 3:2  : PMC = 0b11 (match EL0 + EL1, i.e. user and kernel)
 *   Bits 8:5  : BAS = 0b1111 (byte address select, all 4 bytes of insn)
 *   Bits 13:9 : (reserved / HMC, leave 0)
 *   Bits 15:14: SSC = 0b00
 */
#define BCR_EXEC_ANY	0x000001e7ULL	/* EN=1, PMC=11, BAS=1111 */

/*
 * WCR (Watchpoint Control Register) base: EN=1, PAC=EL0+EL1
 *   Bit  1    : EN = 1
 *   Bits 3:2  : PAC = 0b11 (EL0 + EL1)
 *   Bits 5:4  : (LSC -- Load/Store/Both) -- set by caller
 *   Bits 12:5 : BAS -- set by caller (byte enable)
 */
#define WCR_BASE	0x7ULL		/* EN=1, PAC=11 */

static u64 arm64_watchpoint_bas(u64 addr, int len)
{
	int shift = addr & 7;
	u64 mask;

	if (len <= 0 || len > 8 || shift + len > 8)
		return 0;

	mask = (1ULL << len) - 1;
	return mask << shift;
}

void kvm_gdb__arch_set_debug(struct kvm_cpu *vcpu, bool single_step,
			      struct kvm_gdb_hw_bp *hw_bps)
{
	struct kvm_guest_debug dbg = { 0 };
	int i;

	dbg.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP;

	if (single_step)
		dbg.control |= KVM_GUESTDBG_SINGLESTEP;

	if (hw_bps) {
		bool any_hw = false;
		int bp_idx = 0;	/* hardware breakpoints (exec) use dbg_bvr/bcr */
		int wp_idx = 0;	/* watchpoints use dbg_wvr/wcr */

		for (i = 0; i < 4; i++) {
			if (!hw_bps[i].active)
				continue;

			if (hw_bps[i].type == 0) {
				/* Execution breakpoint (Z1) */
				if (bp_idx >= KVM_ARM_MAX_DBG_REGS)
					continue;
				dbg.arch.dbg_bvr[bp_idx] =
					hw_bps[i].addr & ~3ULL; /* 4-byte align */
				dbg.arch.dbg_bcr[bp_idx] = BCR_EXEC_ANY;
				bp_idx++;
			} else {
				/* Watchpoint: write(1), read(2), access(3) */
				u64 wcr;
				u64 bas;

				if (wp_idx >= KVM_ARM_MAX_DBG_REGS)
					continue;

				/*
				 * BAS: byte-address-select bitmask.
				 * For len=1->0x1, len=2->0x3, len=4->0xf, len=8->0xff.
				 * Encode in WCR bits [12:5].
				 */
				bas = arm64_watchpoint_bas(hw_bps[i].addr,
						   hw_bps[i].len);
				if (!bas)
					continue;

				/*
				 * LSC (Load/Store Control):
				 *   01 = load (read), 10 = store (write),
				 *   11 = load+store (access)
				 * Bits [4:3] of WCR.
				 */
				{
					u64 lsc;

					switch (hw_bps[i].type) {
					case 1:  lsc = 0x2; break;  /* write */
					case 2:  lsc = 0x1; break;  /* read  */
					default: lsc = 0x3; break;  /* access */
					}
					wcr = WCR_BASE |
					      (lsc << 3) |
					      (bas << 5);
				}

				dbg.arch.dbg_wvr[wp_idx] =
					hw_bps[i].addr & ~7ULL; /* 8-byte align */
				dbg.arch.dbg_wcr[wp_idx] = wcr;
				wp_idx++;
			}
			any_hw = true;
		}

		if (any_hw)
			dbg.control |= KVM_GUESTDBG_USE_HW;
	}

	if (ioctl(vcpu->vcpu_fd, KVM_SET_GUEST_DEBUG, &dbg) < 0)
		pr_warning("GDB: KVM_SET_GUEST_DEBUG failed: %s",
			   strerror(errno));
}

void kvm_gdb__arch_prepare_resume(struct kvm_cpu *vcpu, bool single_step,
				   bool from_debug_exit)
{
	u32 pstate;

	if (!single_step || !from_debug_exit)
		return;

	if (get_pstate(vcpu, &pstate) < 0)
		return;

	step_irq_state.vcpu = vcpu;
	step_irq_state.daif_bits = pstate & ARM64_DAIF_MASK;
	step_irq_state.pending = true;

	pstate |= ARM64_DAIF_MASK;
	set_pstate(vcpu, pstate);
}

void kvm_gdb__arch_handle_stop(struct kvm_cpu *vcpu)
{
	u32 pstate;

	if (!step_irq_state.pending || step_irq_state.vcpu != vcpu)
		return;

	if (get_pstate(vcpu, &pstate) < 0)
		return;

	pstate &= ~ARM64_DAIF_MASK;
	pstate |= step_irq_state.daif_bits;
	set_pstate(vcpu, pstate);

	step_irq_state.pending = false;
	step_irq_state.vcpu = NULL;
}

/* ------------------------------------------------------------------ */
/* Stop signal                                                         */
/* ------------------------------------------------------------------ */

int kvm_gdb__arch_signal(struct kvm_cpu *vcpu __attribute__((unused)))
{
	/* All debug exits report SIGTRAP (5) */
	return 5;
}

/* ------------------------------------------------------------------ */
/* Software-breakpoint exit detection and re-injection                 */
/* ------------------------------------------------------------------ */

/*
 * ARM64 debug exits are identified by the EC field in ESR_EL2
 * (reported in kvm_run->debug.arch.hsr).
 *
 *   EC = bits[31:26] of HSR.
 *   0x3C = ESR_ELx_EC_BRK64 -> AArch64 BRK instruction.
 */
bool kvm_gdb__arch_is_sw_bp_exit(struct kvm_cpu *vcpu)
{
	u32 hsr = vcpu->kvm_run->debug.arch.hsr;
	u32 ec  = (hsr >> ESR_EC_SHIFT) & 0x3f;

	return ec == ESR_EC_BRK64;
}

/*
 * Return the guest virtual address of the BRK instruction that triggered
 * the current debug exit.
 *
 * On ARM64, when KVM intercepts a BRK:
 *   - The guest PC has NOT been advanced (no RIP-style auto-increment).
 *   - The PC register (regs.pc) still points at the BRK instruction itself.
 *   - kvm_run->debug.arch.far is the FAR_EL2 value, which is UNKNOWN for
 *     instruction-class exceptions (BRK), so we do NOT use far here.
 *
 * Therefore we read the current PC via KVM_GET_ONE_REG.
 */
u64 kvm_gdb__arch_debug_pc(struct kvm_cpu *vcpu)
{
	return kvm_gdb__arch_get_pc(vcpu);
}

/*
 * Re-inject the BRK exception into the guest so that the guest kernel's own
 * brk_handler (in arch/arm64/kernel/debug-monitors.c) can process it.
 *
 * ARM64 does not support arbitrary exception injection via KVM_SET_VCPU_EVENTS
 * (the ARM64 kvm_vcpu_events struct only has SError).  Instead, we manually
 * simulate what the CPU would do when taking a synchronous exception to EL1:
 *
 *   1. Save current PC -> ELR_EL1          (exception return address)
 *   2. Save current PSTATE -> SPSR_EL1     (saved processor state)
 *   3. Set ESR_EL1 = HSR from the debug exit  (syndrome for brk_handler)
 *   4. Read VBAR_EL1 to find the exception vector base
 *   5. Set PC = VBAR_EL1 + vector_offset   (synchronous exception vector)
 *   6. Set PSTATE = EL1h mode, all interrupts masked
 *
 * Vector offset within VBAR_EL1 (ARM ARM D1.10):
 *   +0x000  current EL, SP_EL0  (PSTATE.EL==1, PSTATE.SP==0)
 *   +0x200  current EL, SP_ELx  (PSTATE.EL==1, PSTATE.SP==1)  <- common kernel
 *   +0x400  lower EL, AArch64   (PSTATE.EL==0)
 *   +0x600  lower EL, AArch32   (not used here)
 *   Synchronous = +0x000 within each quadrant.
 *
 * On failure, we advance PC by 4 to skip the BRK and avoid an infinite loop,
 * accepting that the kernel's BRK handler won't run for this instruction.
 */
void kvm_gdb__arch_reinject_sw_bp(struct kvm_cpu *vcpu)
{
	u64 pc = 0, vbar;
	u32 pstate, hsr;
	bool have_pc = false;
	u64 new_pc;
	u64 vec_off;

	hsr = vcpu->kvm_run->debug.arch.hsr;

	/* Read current PC and PSTATE */
	if (get_one_reg(vcpu, ARM64_CORE_REG(regs.pc), &pc) < 0)
		goto advance_pc;
	have_pc = true;
	if (get_pstate(vcpu, &pstate) < 0)
		goto advance_pc;

	/* Read VBAR_EL1 -- the base of the EL1 exception vector table */
	if (get_one_reg(vcpu, KVM_REG_VBAR_EL1, &vbar) < 0)
		goto advance_pc;

	/* Step 1: ELR_EL1 = current PC (return address = BRK instruction) */
	if (set_one_reg(vcpu, ARM64_CORE_REG(elr_el1), pc) < 0)
		goto advance_pc;

	/* Step 2: SPSR_EL1 = current PSTATE */
	{
		u64 spsr = pstate;
		struct kvm_one_reg reg = {
			.id   = ARM64_CORE_REG(spsr[KVM_SPSR_EL1]),
			.addr = (u64)&spsr,
		};
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0) {
			pr_warning("GDB: reinject: KVM_SET_ONE_REG(spsr) failed: %s",
				   strerror(errno));
			goto advance_pc;
		}
	}

	/*
	 * Step 3: ESR_EL1 = syndrome from the BRK exit.
	 * The HSR value (ESR_EL2 at the time of the VM exit) contains the
	 * correct EC and ISS (BRK immediate) that the kernel's brk_handler
	 * will inspect via read_sysreg(esr_el1).
	 */
	if (set_one_reg(vcpu, KVM_REG_ESR_EL1, (u64)hsr) < 0)
		goto advance_pc;

	/*
	 * Step 4+5: Determine vector offset and set PC.
	 *
	 * PSTATE.EL = bits[3:2], PSTATE.SP = bit[0].
	 */
	{
		u32 el    = (pstate >> 2) & 0x3;
		u32 spsel = pstate & 0x1;

		if (el >= 1) {
			/* From EL1: current EL, SP_ELx or SP_EL0 */
			vec_off = spsel ? 0x200ULL : 0x000ULL;
		} else {
			/* From EL0: lower EL, AArch64 */
			vec_off = 0x400ULL;
		}
	}
	new_pc = vbar + vec_off;
	if (set_one_reg(vcpu, ARM64_CORE_REG(regs.pc), new_pc) < 0)
		goto advance_pc;

	/* Step 6: Set PSTATE = EL1h mode, all interrupts masked */
	if (set_pstate(vcpu, PSR_D_BIT | PSR_A_BIT | PSR_I_BIT |
			     PSR_F_BIT | PSR_MODE_EL1h) < 0)
		goto advance_pc;

	return;

advance_pc:
	/*
	 * Fallback: skip the 4-byte BRK instruction to prevent an infinite
	 * KVM_EXIT_DEBUG loop.  The guest's BRK handler will NOT run.
	 * Only attempt the skip if we successfully read PC; otherwise we
	 * cannot safely advance and just return to avoid writing garbage.
	 */
	if (!have_pc) {
		pr_warning("GDB: reinject_sw_bp failed before PC was read");
		return;
	}
	pr_warning("GDB: reinject_sw_bp failed; skipping BRK at 0x%llx",
		   (unsigned long long)pc);
	set_one_reg(vcpu, ARM64_CORE_REG(regs.pc), pc + 4);
}
