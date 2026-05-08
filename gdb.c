/*
 * GDB Remote Serial Protocol (RSP) stub for kvmtool.
 *
 * Enables debugging a KVM guest via a standard GDB connection,
 * similar to QEMU's -s/-S options.
 *
 * Usage:
 *   lkvm run --gdb 1234 -k bzImage ...   # listen on TCP port 1234
 *   lkvm run --gdb 1234 --gdb-wait ...   # wait for GDB before starting
 *
 *   (gdb) target remote localhost:1234
 *
 * Features:
 *   - Continue / single-step
 *   - Ctrl+C interrupt
 *   - Software breakpoints (Z0/z0) via INT3
 *   - Hardware execution breakpoints (Z1/z1)
 *   - Hardware write/access watchpoints (Z2/z4)
 *   - Multi-vCPU: all vCPUs paused on stop, per-thread register access
 *   - Target XML register description
 */

#include "kvm/gdb.h"

#ifdef CONFIG_ARM64
#include <asm/ptrace.h>
#endif

#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"
#include "kvm/util-init.h"
#include "kvm/mutex.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>

#ifdef CONFIG_ARM64
/*
 * KVM register ID for TTBR1_EL1: S3_0_C2_C0_1
 * op0=3, op1=0, CRn=2, CRm=0, op2=1
 * Built without including arch headers to keep gdb.c architecture-agnostic.
 */
# define GDB_KVM_REG_ARM64		0x6000000000000000ULL
# define GDB_KVM_REG_ARM64_SYSREG	(0x0013ULL << 16)
# define GDB_KVM_REG_SIZE_U64		0x0030000000000000ULL
# define GDB_ARM64_SYSREG(op0,op1,crn,crm,op2) \
	(GDB_KVM_REG_ARM64 | GDB_KVM_REG_SIZE_U64 | GDB_KVM_REG_ARM64_SYSREG | \
	 (((u64)(op0) & 0x3)  << 14) | \
	 (((u64)(op1) & 0x7)  << 11) | \
	 (((u64)(crn) & 0xf)  <<  7) | \
	 (((u64)(crm) & 0xf)  <<  3) | \
	 (((u64)(op2) & 0x7)  <<  0))
# define GDB_KVM_REG_TTBR1_EL1		GDB_ARM64_SYSREG(3, 0, 2, 0, 1)
#endif

#include <linux/kvm.h>

#define GDB_MAX_SW_BP		64
#define GDB_MAX_HW_BP		4
#define GDB_PACKET_MAX		16384

#ifdef CONFIG_ARM64
/*
 * ARM64 software breakpoint: BRK #0 (little-endian 4-byte encoding)
 * Encoding: 0xD4200000  ->  bytes: 0x00 0x00 0x20 0xD4
 */
# define GDB_SW_BP_INSN_LEN	4
static const u8 GDB_SW_BP_INSN[4] = { 0x00, 0x00, 0x20, 0xD4 };
#else
/*
 * x86 software breakpoint: INT3 (1-byte opcode 0xCC)
 */
# define GDB_SW_BP_INSN_LEN	1
static const u8 GDB_SW_BP_INSN[1] = { 0xCC };
#endif

/*
 * Only use raw address-as-GPA fallback for very low addresses where
 * real-mode/early-boot identity mapping is plausible.
 */
#define GDB_IDMAP_FALLBACK_MAX	0x100000ULL

/* Software breakpoint saved state */
struct sw_bp {
	u64  addr;
	u8   orig_bytes[GDB_SW_BP_INSN_LEN];	/* original instruction bytes */
	int  refs;
	bool active;
};

/*
 * All GDB stub state lives here.
 * Accesses must be done with gdb.lock held, except where noted.
 */
static struct kvm_gdb {
	int		 port;
	int		 listen_fd;
	int		 fd;		/* Connected GDB fd, -1 if none */
	bool		 active;	/* Stub is configured */
	bool		 wait;		/* --gdb-wait: block until GDB connects */
	bool		 connected;	/* A GDB client is currently connected */

	struct kvm	*kvm;
	pthread_t	 thread;

	/* vCPU <-> GDB thread synchronisation */
	pthread_mutex_t	lock;
	pthread_cond_t	vcpu_stopped;	/* vCPU -> GDB: we hit a debug event */
	pthread_cond_t	vcpu_resume;	/* GDB -> vCPU: you may run again */

	/*
	 * Set by vCPU thread when it enters debug handling.
	 * Cleared when GDB signals vcpu_resume.
	 */
	struct kvm_cpu	*stopped_vcpu;

	/* Currently selected thread for Hg / Hc commands (-1 = any) */
	int		 g_tid;		/* register ops */
	int		 c_tid;		/* step/continue */

	/* Breakpoints */
	struct sw_bp		sw_bp[GDB_MAX_SW_BP];
	struct kvm_gdb_hw_bp	hw_bp[GDB_MAX_HW_BP];

	/* If true we are about to single-step the current vCPU */
	bool		 single_step;

	/* Used to wait for GDB connection before starting vCPUs */
	pthread_cond_t	 connected_cond;
} gdb = {
	.fd          = -1,
	.listen_fd   = -1,
	.g_tid       = -1,
	.c_tid       = -1,
	.lock        = PTHREAD_MUTEX_INITIALIZER,
	.vcpu_stopped   = PTHREAD_COND_INITIALIZER,
	.vcpu_resume    = PTHREAD_COND_INITIALIZER,
	.connected_cond = PTHREAD_COND_INITIALIZER,
};

struct sw_bp_resume {
	int	idx;
	u64	addr;
	bool	active;
	bool	auto_resume;
};

static struct sw_bp_resume sw_bp_resume = {
	.idx = -1,
};

static bool gdb_write_guest_mem(u64 addr, const void *buf, size_t len);
static bool gdb_write_guest_insn(u64 addr, const void *buf, size_t len);
static struct kvm_cpu *current_vcpu(void);

/* ------------------------------------------------------------------ */
/* Utility: hex / binary conversion                                    */
/* ------------------------------------------------------------------ */

static const char hex_chars[] = "0123456789abcdef";

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void bin_to_hex(const void *bin, size_t len, char *hex)
{
	const u8 *b = bin;
	size_t i;

	for (i = 0; i < len; i++) {
		hex[i * 2]     = hex_chars[b[i] >> 4];
		hex[i * 2 + 1] = hex_chars[b[i] & 0xf];
	}
}

/* Returns number of bytes written, or -1 on invalid hex. */
static int hex_to_bin(const char *hex, size_t hexlen, void *bin)
{
	u8 *b = bin;
	size_t i;

	if (hexlen & 1)
		return -1;
	for (i = 0; i < hexlen / 2; i++) {
		int hi = hex_nibble(hex[i * 2]);
		int lo = hex_nibble(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		b[i] = (u8)((hi << 4) | lo);
	}
	return hexlen / 2;
}

static int gdb_unescape_binary(const char *in, size_t in_len, void *out,
			       size_t out_len)
{
	const u8 *src = (const u8 *)in;
	u8 *dst = out;
	size_t i = 0, j = 0;

	while (i < in_len && j < out_len) {
		u8 ch = src[i++];

		if (ch == '}') {
			if (i >= in_len)
				return -1;
			ch = src[i++] ^ 0x20;
		}

		dst[j++] = ch;
	}

	return (i == in_len && j == out_len) ? 0 : -1;
}

/* Parse a hex number from *p, advancing *p past the digits. */
static u64 parse_hex(const char **p)
{
	u64 val = 0;
	while (**p && hex_nibble(**p) >= 0) {
		val = (val << 4) | hex_nibble(**p);
		(*p)++;
	}
	return val;
}

/* ------------------------------------------------------------------ */
/* Packet I/O                                                          */
/* ------------------------------------------------------------------ */

/*
 * Read exactly one byte from fd.
 * Returns the byte value [0..255] or -1 on error/EOF.
 */
static int gdb_read_byte(int fd)
{
	unsigned char c;
	ssize_t r = read(fd, &c, 1);
	if (r <= 0)
		return -1;
	return c;
}

/*
 * Receive one GDB RSP packet.
 * Skips leading junk until '$', reads data until '#', reads 2-char checksum.
 * Returns:
 *   >= 0  number of bytes in buf (NUL-terminated)
 *   -1    I/O error or disconnect
 *   -2    Ctrl+C received (0x03 interrupt byte)
 */
static int gdb_recv_packet(int fd, char *buf, size_t bufsz)
{
	int c;
	size_t len;
	u8 cksum;
	int cs_hi;
	int cs_lo;
	u8 expected;
	char nak;
	char ack;

retry:
	/* Scan for '$' or 0x03 */
	do {
		c = gdb_read_byte(fd);
		if (c < 0)
			return -1;
		if (c == 0x03)
			return -2;
	} while (c != '$');

	/* Read packet data */
	len = 0;
	cksum = 0;
	while (1) {
		c = gdb_read_byte(fd);
		if (c < 0)
			return -1;
		if (c == '#')
			break;
		if (len + 1 >= bufsz)
			return -1;	/* overflow */
		buf[len++] = (char)c;
		cksum += (u8)c;
	}
	buf[len] = '\0';

	/* Read 2-digit checksum from client */
	cs_hi = gdb_read_byte(fd);
	cs_lo = gdb_read_byte(fd);
	if (cs_hi < 0 || cs_lo < 0)
		return -1;

	expected = (u8)((hex_nibble(cs_hi) << 4) | hex_nibble(cs_lo));
	if (expected != cksum) {
		/* Checksum mismatch: NAK and retry (best-effort send) */
		nak = '-';
		if (write(fd, &nak, 1) < 0)
			return -1;
		goto retry;
	}

	/* ACK (best-effort send) */
	ack = '+';
	if (write(fd, &ack, 1) < 0)
		return -1;

	return (int)len;
}

/*
 * Send a GDB RSP packet "$data#checksum".
 * data must be a NUL-terminated string.
 * Returns 0 on success, -1 on error.
 */
static int gdb_send_packet(int fd, const char *data)
{
	size_t len = strlen(data);
	size_t i;
	u8 cksum = 0;
	char trailer[4];
	char header;
	char ack;

	for (i = 0; i < len; i++)
		cksum += (u8)data[i];

	snprintf(trailer, sizeof(trailer), "#%02x", cksum);

	/* We send as three separate writes to avoid a heap allocation.
	 * Small enough that no buffering is needed. */
	header = '$';
	if (write(fd, &header, 1)   != 1 ||
	    write(fd, data, len)    != (ssize_t)len ||
	    write(fd, trailer, 3)   != 3)
		return -1;

	/* Consume the ACK/NAK (best-effort; ignore NACK) */
	if (read(fd, &ack, 1) != 1)
		return -1;
	return 0;
}

static void gdb_send_ok(int fd)
{
	gdb_send_packet(fd, "OK");
}

static void gdb_send_error(int fd, int err)
{
	char buf[8];
	snprintf(buf, sizeof(buf), "E%02x", err & 0xff);
	gdb_send_packet(fd, buf);
}

static void gdb_send_empty(int fd)
{
	gdb_send_packet(fd, "");
}

/* ------------------------------------------------------------------ */
/* vCPU selection helpers                                              */
/* ------------------------------------------------------------------ */

/* Convert a GDB thread-ID string to a vCPU index (0-based).
 * GDB thread IDs are 1-based (thread 1 = vCPU 0).
 * Returns -1 on "all threads" or parse error, or the vCPU index.
 */
static int tid_to_vcpu(const char *s)
{
	const char *p = s;
	long tid;

	if (s[0] == '-' && s[1] == '1')
		return -1;	/* "all threads" */
	if (!*p)
		return -2;
	/* GDB may send hex thread IDs; parse as hex */
	tid = (long)parse_hex(&p);
	if (*p != '\0' || tid <= 0)
		return -2;
	return (int)(tid - 1);
}

static int sw_bp_find(u64 addr)
{
	int i;

	for (i = 0; i < GDB_MAX_SW_BP; i++) {
		if (gdb.sw_bp[i].active && gdb.sw_bp[i].addr == addr)
			return i;
	}

	return -1;
}

static int sw_bp_restore(int idx)
{
	if (idx < 0 || idx >= GDB_MAX_SW_BP || !gdb.sw_bp[idx].active)
		return -1;

	return gdb_write_guest_insn(gdb.sw_bp[idx].addr,
				   gdb.sw_bp[idx].orig_bytes,
				   GDB_SW_BP_INSN_LEN) ? 0 : -1;
}

static int sw_bp_reinsert(int idx)
{
	if (idx < 0 || idx >= GDB_MAX_SW_BP || !gdb.sw_bp[idx].active)
		return -1;

	return gdb_write_guest_insn(gdb.sw_bp[idx].addr,
				   GDB_SW_BP_INSN,
				   GDB_SW_BP_INSN_LEN) ? 0 : -1;
}

static bool prepare_sw_bp_resume(bool auto_resume)
{
	struct kvm_cpu *vcpu = current_vcpu();
	u64 bp_addr;
	int idx;

	if (!vcpu || !kvm_gdb__arch_is_sw_bp_exit(vcpu))
		return false;

	bp_addr = kvm_gdb__arch_debug_pc(vcpu);
	idx = sw_bp_find(bp_addr);
	if (idx < 0)
		return false;

	if (sw_bp_restore(idx) < 0)
		return false;

	gdb.sw_bp[idx].active = false;
	sw_bp_resume.idx = idx;
	sw_bp_resume.addr = bp_addr;
	sw_bp_resume.active = true;
	sw_bp_resume.auto_resume = auto_resume;

	return true;
}

static bool finish_sw_bp_resume(bool *auto_resume)
{
	int idx;

	if (!sw_bp_resume.active)
		return false;

	idx = sw_bp_resume.idx;
	if (idx >= 0 && idx < GDB_MAX_SW_BP) {
		gdb.sw_bp[idx].active = true;
		sw_bp_reinsert(idx);
	}

	*auto_resume = sw_bp_resume.auto_resume;
	sw_bp_resume.idx = -1;
	sw_bp_resume.active = false;
	return true;
}

#if !defined(CONFIG_X86) && !defined(CONFIG_ARM64)
void kvm_gdb__arch_read_registers(struct kvm_cpu *vcpu, u8 *buf, size_t *size)
{
	*size = 0;
}

void kvm_gdb__arch_write_registers(struct kvm_cpu *vcpu, const u8 *buf,
				    size_t size)
{
}

int kvm_gdb__arch_read_register(struct kvm_cpu *vcpu, int regno,
				u8 *buf, size_t *size)
{
	return -1;
}

int kvm_gdb__arch_write_register(struct kvm_cpu *vcpu, int regno,
				 const u8 *buf, size_t size)
{
	return -1;
}

u64 kvm_gdb__arch_get_pc(struct kvm_cpu *vcpu)
{
	return 0;
}

void kvm_gdb__arch_set_pc(struct kvm_cpu *vcpu, u64 pc)
{
}

void kvm_gdb__arch_set_debug(struct kvm_cpu *vcpu, bool single_step,
			      struct kvm_gdb_hw_bp *hw_bps)
{
}

void kvm_gdb__arch_prepare_resume(struct kvm_cpu *vcpu, bool single_step,
				   bool from_debug_exit)
{
}

void kvm_gdb__arch_handle_stop(struct kvm_cpu *vcpu)
{
}

const char *kvm_gdb__arch_target_xml(void)
{
	return NULL;
}

size_t kvm_gdb__arch_reg_pkt_size(void)
{
	return 0;
}

int kvm_gdb__arch_signal(struct kvm_cpu *vcpu)
{
	return 5;
}

bool kvm_gdb__arch_is_sw_bp_exit(struct kvm_cpu *vcpu)
{
	return false;
}

u64 kvm_gdb__arch_debug_pc(struct kvm_cpu *vcpu)
{
	return 0;
}

void kvm_gdb__arch_reinject_sw_bp(struct kvm_cpu *vcpu)
{
}
#endif

/* Return the vCPU pointer for the currently selected thread (g_tid).
 * Falls back to vCPU 0.
 */
static struct kvm_cpu *current_vcpu(void)
{
	int idx;

	if (gdb.stopped_vcpu)
		return gdb.stopped_vcpu;

	idx = (gdb.g_tid <= 0) ? 0 : (gdb.g_tid - 1);
	if (idx >= gdb.kvm->nrcpus)
		idx = 0;
	return gdb.kvm->cpus[idx];
}

/* ------------------------------------------------------------------ */
/* Guest memory access                                                 */
/* ------------------------------------------------------------------ */

/*
 * Linux x86-64 virtual address space constants.
 * Used as a last-resort fallback when KVM_TRANSLATE fails.
 *
 * __START_KERNEL_map (0xffffffff80000000):
 *   Maps physical RAM starting from 0.  With nokaslr the kernel binary
 *   is loaded at physical 0x1000000 and linked at 0xffffffff81000000.
 *   Formula: GPA = GVA - __START_KERNEL_map
 *
 * PAGE_OFFSET / direct-map (0xffff888000000000):
 *   Direct 1:1 mapping of all physical RAM.
 *   Formula: GPA = GVA - PAGE_OFFSET
 *   This offset is fixed in the x86-64 ABI regardless of KASLR.
 */
#ifdef CONFIG_X86
/*
 * x86-64 Linux kernel virtual address layout (with nokaslr):
 *   __START_KERNEL_map  0xffffffff80000000  kernel text, GPA = GVA - base
 *   PAGE_OFFSET         0xffff888000000000  direct phys map, GPA = GVA - base
 */
# define GDB_KERNEL_MAP_BASE	0xffffffff80000000ULL
# define GDB_DIRECT_MAP_BASE	0xffff888000000000ULL
# define GDB_DIRECT_MAP_SIZE	0x100000000000ULL	/* 16 TB */
#endif

#ifdef CONFIG_ARM64
/*
 * ARM64 Linux kernel virtual address layout:
 *
 * Linear map (PAGE_OFFSET):
 *   The kernel maps all physical RAM at PAGE_OFFSET.  The exact value
 *   depends on VA_BITS (48 or 52), but for a standard kernel with VA_BITS=48:
 *     PAGE_OFFSET = 0xffff000000000000
 *   With VA_BITS=39 (some embedded configs):
 *     PAGE_OFFSET = 0xffffff8000000000
 *   Formula: GPA = GVA - PAGE_OFFSET
 *
 * Kernel text / vmalloc (KIMAGE_VADDR):
 *   Standard arm64 kernel is linked at 0xffff800008000000 (VA_BITS=48).
 *   The kernel image occupies [KIMAGE_VADDR, KIMAGE_VADDR + TEXT_OFFSET + size).
 *   For kvmtool guests, the default load address is usually 0x80000 (physical),
 *   so kernel text GPA ~= GVA - 0xffff800008000000 + 0x80000
 *   = GVA - 0xffff800007f80000.
 *
 *   Simpler approximation: treat the full vmalloc/kernel range as a linear
 *   region from 0xffff800000000000 onward, with offset 0xffff800000000000 -
 *   PHYS_OFFSET where PHYS_OFFSET is typically 0x40000000 on kvmtool guests.
 *
 * In practice, KVM_TRANSLATE works correctly when the vCPU is paused in EL1
 * (kernel mode).  The fallback is only needed when the vCPU is paused in EL0
 * (userspace) with TTBR1_EL1 loaded but active stage-1 translation using
 * TTBR0_EL1 (user page table) which does not cover kernel addresses.
 *
 * We use the same strategy as x86: check for the well-known linear map range
 * first, then fall back to the kernel image range.
 *
 * PAGE_OFFSET for VA_BITS=48:  0xffff000000000000
 * All kernel virtual addresses are >= 0xffff000000000000.
 * kvmtool maps guest RAM at IPA 0x80000000 (ARM_MEMORY_AREA).
 *
 * Linear map formula:  GPA = GVA - 0xffff000000000000 + ARM_MEMORY_AREA
 *   (kvmtool's physical memory slot starts at IPA 0x80000000.  See arm/kvm.c.)
 *
 * Kernel image formula: GPA = GVA - 0xffff800008000000 + 0x80000
 *   Approximated as:    GPA = GVA - 0xffff800007f80000
 *
 * Because these offsets vary by kernel config, this fallback is a best-effort
 * heuristic; use nokaslr and ensure the vCPU is in EL1 for reliable results.
 */

/* VA_BITS=48 linear map base (PAGE_OFFSET) */
# define GDB_ARM64_PAGE_OFFSET		0xffff000000000000ULL
/* kvmtool ARM64 guest RAM starts at IPA 0x80000000 (ARM_MEMORY_AREA) */
# define GDB_ARM64_PHYS_OFFSET		0x80000000ULL
# define GDB_ARM64_LINEAR_MAP_SIZE	0x1000000000000ULL  /* 256 TB region */

/* Kernel image virtual base (KIMAGE_VADDR, VA_BITS=48) */
# define GDB_ARM64_KIMAGE_VADDR		0xffff800008000000ULL
/* TEXT_OFFSET: read from kernel image header; 0x0 for newer kernels, 0x80000 for older */
# define GDB_ARM64_TEXT_OFFSET		0x0ULL

/*
 * arm64_sw_walk_ttbr1() - software walk of the kernel stage-1 page table.
 *
 * KVM_TRANSLATE is not implemented on ARM64 (returns ENXIO).  Instead we
 * manually walk the TTBR1_EL1 4-level page table that the guest kernel uses
 * for all kernel virtual addresses (bit[55] == 1, i.e. TTBR1 range).
 *
 * Supports 4KB granule, VA_BITS=48 (the most common arm64 Linux config):
 *   Level 0 (PGD): bits [47:39]  ->  9 bits, 512 entries
 *   Level 1 (PUD): bits [38:30]  ->  9 bits, 512 entries
 *   Level 2 (PMD): bits [29:21]  ->  9 bits, 512 entries
 *   Level 3 (PTE): bits [20:12]  ->  9 bits, 512 entries
 *   Page offset:   bits [11:0]   -> 12 bits
 *
 * Each entry is 8 bytes.  Bits [47:12] of a non-block entry hold the next
 * table's IPA (= GPA in kvmtool's flat Stage-2 identity map).
 *
 * Block entries:
 *   L1 block: 1 GB,  output address = entry[47:30] << 30
 *   L2 block: 2 MB,  output address = entry[47:21] << 21
 *
 * Entry validity:
 *   bit[0] == 1:  valid
 *   bit[1] == 1:  table (if at L0/L1/L2), page (if at L3)
 *   bit[1] == 0:  block (if at L1/L2), reserved (if at L0)
 *
 * Returns the GPA on success, (u64)-1 on failure.
 */
static u64 arm64_sw_walk_ttbr1(u64 gva)
{
	struct kvm_cpu *cur = current_vcpu();
	struct kvm_one_reg reg;
	u64 ttbr1;
	u64 tbl;
	int shifts[4] = { 39, 30, 21, 12 };
	u64 masks[4]  = { 0x1ff, 0x1ff, 0x1ff, 0x1ff };
	int level;

	if (!cur) {
		pr_warning("GDB: arm64_walk: no current_vcpu");
		return (u64)-1;
	}

	/*
	 * Read TTBR1_EL1.  The ASID field is in bits [63:48]; the base
	 * address is in bits [47:1] (BADDR), effectively [47:12] for 4KB
	 * granule after masking ASID and CnP.
	 */
	reg.id   = GDB_KVM_REG_TTBR1_EL1;
	reg.addr = (u64)&ttbr1;
	if (ioctl(cur->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0) {
		pr_warning("GDB: arm64_walk: KVM_GET_ONE_REG(TTBR1_EL1) failed: %s",
			   strerror(errno));
		return (u64)-1;
	}

	/* Strip ASID (bits [63:48]) and CnP (bit[0]) to get table base GPA */
	tbl = ttbr1 & 0x0000fffffffff000ULL;

	pr_debug("GDB: arm64_walk GVA=0x%llx TTBR1=0x%llx tbl=0x%llx",
		 (unsigned long long)gva,
		 (unsigned long long)ttbr1,
		 (unsigned long long)tbl);

	/* VA bits for each level (4KB granule, VA_BITS=48) */
	for (level = 0; level < 4; level++) {
		u64 idx       = (gva >> shifts[level]) & masks[level];
		u64 entry_gpa = tbl + idx * 8;
		u64 pte;
		u8  *host;

		/* Read the 8-byte page-table entry from guest memory */
		host = guest_flat_to_host(gdb.kvm, entry_gpa);
		if (!host || !host_ptr_in_ram(gdb.kvm, host) ||
		    !host_ptr_in_ram(gdb.kvm, host + 7)) {
			pr_warning("GDB: arm64_walk L%d: entry_gpa=0x%llx not in RAM (tbl=0x%llx idx=%llu)",
				   level,
				   (unsigned long long)entry_gpa,
				   (unsigned long long)tbl,
				   (unsigned long long)idx);
			return (u64)-1;
		}

		memcpy(&pte, host, 8);

		pr_debug("GDB: arm64_walk L%d idx=%llu entry_gpa=0x%llx pte=0x%llx",
			 level, (unsigned long long)idx,
			 (unsigned long long)entry_gpa,
			 (unsigned long long)pte);

		/* Entry must be valid (bit[0]) */
		if (!(pte & 1ULL)) {
			pr_warning("GDB: arm64_walk L%d: pte=0x%llx not valid",
				   level, (unsigned long long)pte);
			return (u64)-1;
		}

		if (level == 3) {
			/* L3 page entry: output address = pte[47:12] */
			u64 pa = (pte & 0x0000fffffffff000ULL) |
				 (gva & 0xfffULL);
			pr_debug("GDB: arm64_walk -> PA=0x%llx", (unsigned long long)pa);
			return pa;
		}

		/* bit[1]: 0 = block, 1 = table */
		if (!(pte & 2ULL)) {
			u64 pa;

			/* Block entry at L1 (1GB) or L2 (2MB) */
			if (level == 1) {
				pa = (pte & 0x0000ffffc0000000ULL) |
				     (gva & 0x3fffffffULL);
				pr_debug("GDB: arm64_walk L1 block -> PA=0x%llx", (unsigned long long)pa);
				return pa;
			} else if (level == 2) {
				pa = (pte & 0x0000ffffffe00000ULL) |
				     (gva & 0x1fffffULL);
				pr_debug("GDB: arm64_walk L2 block -> PA=0x%llx", (unsigned long long)pa);
				return pa;
			}
			/* L0 block is reserved */
			pr_warning("GDB: arm64_walk L%d: unexpected block entry", level);
			return (u64)-1;
		}

		/* Table entry: next level base = pte[47:12] */
		tbl = pte & 0x0000fffffffff000ULL;
	}

	return (u64)-1;
}
#endif

/*
 * Translate a guest virtual address (GVA) to a guest physical address (GPA).
 *
 * Uses three strategies in order:
 *
 * 1. KVM_TRANSLATE on the currently selected vCPU.
 *    Fails when the vCPU was paused in user mode (Linux KPTI / ARM64 TTBR0)
 *    because the user-mode page table does not map kernel addresses.
 *
 * 2. KVM_TRANSLATE on every other vCPU.
 *    On multi-vCPU systems, another vCPU may be paused in kernel mode
 *    whose page tables include kernel mappings.
 *
 * 3. Fixed-offset arithmetic for well-known Linux kernel ranges.
 *    This is the safety net for single-vCPU systems where ALL vCPUs are
 *    paused in user mode (common when debugging a booted VM running a
 *    shell).  Only reliable with the nokaslr kernel parameter.
 *
 * Returns the GPA on success, or (u64)-1 on failure.
 */
static u64 gva_to_gpa(u64 gva)
{
	struct kvm_cpu *cur = current_vcpu();
	int i;

	/* Strategy 1: KVM_TRANSLATE on the preferred vCPU */
	if (cur) {
		struct kvm_translation trans = { .linear_address = gva };
		if (ioctl(cur->vcpu_fd, KVM_TRANSLATE, &trans) == 0 &&
		    trans.valid)
			return trans.physical_address;
	}

	/*
	 * Strategy 2: try every other vCPU.
	 *
	 * x86 Linux KPTI / ARM64: user-mode page tables do NOT map kernel
	 * virtual addresses.  If the selected vCPU was interrupted while
	 * running a userspace process, a different vCPU that was paused inside
	 * the kernel will have the kernel-mode page table loaded and can
	 * translate kernel addresses successfully.
	 */
	for (i = 0; i < gdb.kvm->nrcpus; i++) {
		struct kvm_cpu *vcpu = gdb.kvm->cpus[i];
		struct kvm_translation trans;

		if (vcpu == cur)
			continue;
		trans.linear_address = gva;
		if (ioctl(vcpu->vcpu_fd, KVM_TRANSLATE, &trans) == 0 &&
		    trans.valid)
			return trans.physical_address;
	}

#ifdef CONFIG_X86
	/*
	 * Strategy 3 (x86-64): fixed-offset fallback for Linux kernel ranges.
	 *
	 * When ALL vCPUs are paused in user mode (e.g. a single-vCPU VM
	 * running a shell), KVM_TRANSLATE will fail for every kernel address.
	 *
	 * Direct physical map (PAGE_OFFSET): always fixed, KASLR-safe.
	 * Kernel text/data (__START_KERNEL_map): fixed only with nokaslr.
	 */
	if (gva >= GDB_DIRECT_MAP_BASE &&
	    gva <  GDB_DIRECT_MAP_BASE + GDB_DIRECT_MAP_SIZE)
		return gva - GDB_DIRECT_MAP_BASE;

	if (gva >= GDB_KERNEL_MAP_BASE)
		return gva - GDB_KERNEL_MAP_BASE;
#endif

#ifdef CONFIG_ARM64
	/*
	 * Strategy 3 (ARM64): software page-table walk via TTBR1_EL1.
	 *
	 * KVM_TRANSLATE is NOT implemented on ARM64 (always returns ENXIO).
	 * Instead we read TTBR1_EL1 (kernel page-table base) and walk the
	 * stage-1 4-level page table in software using guest_flat_to_host()
	 * to access guest memory.
	 *
	 * This works correctly regardless of KASLR or non-standard PHYS_OFFSET,
	 * as long as:
	 *   - The vCPU has TTBR1_EL1 configured (true after MMU is enabled).
	 *   - kvmtool's stage-2 IPA->GPA mapping is a flat identity (it is).
	 *   - The granule is 4KB with VA_BITS=48 (standard arm64 Linux).
	 *
	 * Fallback to fixed-offset arithmetic is kept for early boot (MMU off)
	 * or unusual kernel configs.
	 */
	if (gva >= 0xffff000000000000ULL) {
		u64 gpa = arm64_sw_walk_ttbr1(gva);
		if (gpa != (u64)-1)
			return gpa;
	}

	/*
	 * Fixed-offset fallback (best-effort, requires nokaslr):
	 *
	 *   Linear map  [0xffff000000000000, 0xffff000000000000 + 256TB):
	 *     GPA = GVA - PAGE_OFFSET + PHYS_OFFSET
	 *   Kernel image [0xffff800000000000, ...):
	 *     GPA = GVA - KIMAGE_VADDR + TEXT_OFFSET + PHYS_OFFSET
	 *
	 * These constants match VA_BITS=48, 4KB granule, kvmtool default
	 * PHYS_OFFSET=0x40000000, TEXT_OFFSET=0x80000.
	 */

	/* Linear map range: [PAGE_OFFSET, PAGE_OFFSET + LINEAR_MAP_SIZE) */
	if (gva >= GDB_ARM64_PAGE_OFFSET &&
	    gva <  GDB_ARM64_PAGE_OFFSET + GDB_ARM64_LINEAR_MAP_SIZE)
		return gva - GDB_ARM64_PAGE_OFFSET + GDB_ARM64_PHYS_OFFSET;

	/* Kernel image / vmalloc range: [0xffff800000000000, ...) */
	if (gva >= GDB_ARM64_KIMAGE_VADDR)
		return gva - GDB_ARM64_KIMAGE_VADDR
		       + GDB_ARM64_TEXT_OFFSET
		       + GDB_ARM64_PHYS_OFFSET;
#endif

	return (u64)-1;
}

/*
 * Read/write guest memory at a guest virtual address.
 * Handles page-boundary crossing and GVA->GPA translation.
 * Falls back to treating the address as a GPA if translation fails.
 */
static bool gdb_read_guest_mem(u64 addr, void *buf, size_t len)
{
	u8 *out = buf;

	while (len > 0) {
		u64 gpa = gva_to_gpa(addr);
		size_t page_rem;
		size_t chunk;
		u8 *host;

		/*
		 * Only fall back to treating addr as GPA for low (real-mode /
		 * identity-mapped) addresses.  For kernel virtual addresses
		 * (above 2GB) the fallback would produce a wildly wrong GPA
		 * and cause guest_flat_to_host() to print a spurious warning.
		 */
		if (gpa == (u64)-1) {
			if (addr < GDB_IDMAP_FALLBACK_MAX)
				gpa = addr;	/* real-mode identity mapping */
			else
				return false;
		}

		/* Clamp transfer to the current page */
		page_rem = 0x1000 - (gpa & 0xfff);
		chunk = (page_rem < len) ? page_rem : len;

		host = guest_flat_to_host(gdb.kvm, gpa);
		if (!host || !host_ptr_in_ram(gdb.kvm, host) ||
		    !host_ptr_in_ram(gdb.kvm, host + chunk - 1))
			return false;

		memcpy(out, host, chunk);
		out  += chunk;
		addr += chunk;
		len  -= chunk;
	}
	return true;
}

static bool gdb_write_guest_mem_internal(u64 addr, const void *buf, size_t len,
					 bool sync_icache)
{
	const u8 *in = buf;

	while (len > 0) {
		u64 gpa = gva_to_gpa(addr);
		size_t page_rem;
		size_t chunk;
		u8 *host;

		if (gpa == (u64)-1) {
			if (addr < GDB_IDMAP_FALLBACK_MAX)
				gpa = addr;
			else
				return false;
		}

		page_rem = 0x1000 - (gpa & 0xfff);
		chunk = (page_rem < len) ? page_rem : len;

		host = guest_flat_to_host(gdb.kvm, gpa);
		if (!host || !host_ptr_in_ram(gdb.kvm, host) ||
		    !host_ptr_in_ram(gdb.kvm, host + chunk - 1))
			return false;

		memcpy(host, in, chunk);
		if (sync_icache)
			kvm_gdb__arch_sync_guest_insn(host, chunk);
		in   += chunk;
		addr += chunk;
		len  -= chunk;
	}
	return true;
}

static bool gdb_write_guest_mem(u64 addr, const void *buf, size_t len)
{
	return gdb_write_guest_mem_internal(addr, buf, len, false);
}

static bool gdb_write_guest_insn(u64 addr, const void *buf, size_t len)
{
	return gdb_write_guest_mem_internal(addr, buf, len, true);
}

/* ------------------------------------------------------------------ */
/* Software breakpoints                                                */
/* ------------------------------------------------------------------ */

static int sw_bp_insert(u64 addr, int len)
{
	int i;

	for (i = 0; i < GDB_MAX_SW_BP; i++) {
		if (gdb.sw_bp[i].refs > 0 && gdb.sw_bp[i].addr == addr) {
			gdb.sw_bp[i].refs++;
			return 0;
		}
	}

	/* Find a free slot */
	for (i = 0; i < GDB_MAX_SW_BP; i++) {
		if (gdb.sw_bp[i].refs > 0)
			continue;

		if (!gdb_read_guest_mem(addr, gdb.sw_bp[i].orig_bytes,
					GDB_SW_BP_INSN_LEN)) {
			pr_warning("GDB: sw_bp_insert read failed at GVA 0x%llx",
				   (unsigned long long)addr);
			return -1;
		}
		if (!gdb_write_guest_insn(addr, GDB_SW_BP_INSN,
					  GDB_SW_BP_INSN_LEN)) {
			pr_warning("GDB: sw_bp_insert write failed at GVA 0x%llx",
				   (unsigned long long)addr);
			return -1;
		}

		gdb.sw_bp[i].addr   = addr;
		gdb.sw_bp[i].refs   = 1;
		gdb.sw_bp[i].active = true;
		return 0;
	}
	return -1;	/* table full */
}

static int sw_bp_remove(u64 addr, int len)
{
	int i;

	for (i = 0; i < GDB_MAX_SW_BP; i++) {
		if (gdb.sw_bp[i].refs <= 0 || gdb.sw_bp[i].addr != addr)
			continue;

		if (--gdb.sw_bp[i].refs > 0)
			return 0;

		if (gdb.sw_bp[i].active)
			gdb_write_guest_insn(addr, gdb.sw_bp[i].orig_bytes,
				     GDB_SW_BP_INSN_LEN);
		gdb.sw_bp[i].active = false;
		return 0;
	}
	return -1;
}

/* Return true if there is an active software breakpoint at addr. */
static bool sw_bp_active_at(u64 addr)
{
	int i;

	for (i = 0; i < GDB_MAX_SW_BP; i++) {
		if (gdb.sw_bp[i].active && gdb.sw_bp[i].addr == addr)
			return true;
	}
	return false;
}

/* Remove all software breakpoints before resuming the guest. */
static void sw_bp_remove_all(void)
{
	int i;

	for (i = 0; i < GDB_MAX_SW_BP; i++) {
		if (gdb.sw_bp[i].refs <= 0)
			continue;
		if (gdb.sw_bp[i].active)
			gdb_write_guest_insn(gdb.sw_bp[i].addr,
					     gdb.sw_bp[i].orig_bytes,
					     GDB_SW_BP_INSN_LEN);
		gdb.sw_bp[i].refs = 0;
		gdb.sw_bp[i].active = false;
	}
}

/* ------------------------------------------------------------------ */
/* Hardware breakpoints / watchpoints                                  */
/* ------------------------------------------------------------------ */

static int hw_bp_insert(int type, u64 addr, int len)
{
	int i;

	for (i = 0; i < GDB_MAX_HW_BP; i++) {
		if (!gdb.hw_bp[i].active) {
			gdb.hw_bp[i].addr   = addr;
			gdb.hw_bp[i].len    = len;
			gdb.hw_bp[i].type   = type;
			gdb.hw_bp[i].active = true;
			return 0;
		}
	}
	return -1;
}

static int hw_bp_remove(int type, u64 addr, int len)
{
	int i;

	for (i = 0; i < GDB_MAX_HW_BP; i++) {
		if (gdb.hw_bp[i].active &&
		    gdb.hw_bp[i].addr == (u64)addr &&
		    gdb.hw_bp[i].type == type) {
			gdb.hw_bp[i].active = false;
			return 0;
		}
	}
	return -1;
}

/*
 * Apply current debug configuration to all vCPUs.
 * Only step_vcpu gets KVM_GUESTDBG_SINGLESTEP; all others keep breakpoint
 * interception active but run without TF set.
 */
static void apply_debug_to_all(struct kvm_cpu *step_vcpu, bool single_step)
{
	int i;

	for (i = 0; i < gdb.kvm->nrcpus; i++)
		kvm_gdb__arch_set_debug(gdb.kvm->cpus[i],
					gdb.kvm->cpus[i] == step_vcpu && single_step,
					gdb.hw_bp);
}

/* ------------------------------------------------------------------ */
/* Stop reply                                                          */
/* ------------------------------------------------------------------ */

/*
 * Send a "T" stop-reply packet:
 *   T<sig>thread:<tid>;
 * where <sig> = SIGTRAP (5) in hex.
 */
static void gdb_send_stop_reply(int fd, struct kvm_cpu *vcpu)
{
	int sig = kvm_gdb__arch_signal(vcpu);
	int tid = (int)(vcpu->cpu_id + 1);

	char buf[80];
	/* Include swbreak: since we advertise swbreak+ in qSupported */
	if (kvm_gdb__arch_is_sw_bp_exit(vcpu))
		snprintf(buf, sizeof(buf), "T%02xswbreak:;thread:%x;", sig, tid);
	else
		snprintf(buf, sizeof(buf), "T%02xthread:%x;", sig, tid);
	gdb_send_packet(fd, buf);
}

/* ------------------------------------------------------------------ */
/* qXfer: features                                                     */
/* ------------------------------------------------------------------ */

/*
 * Handle qXfer:features:read:target.xml:offset,length
 * Returns true if handled.
 */
static bool handle_qxfer_features(int fd, const char *annex,
				   u64 offset, u64 reqlen)
{
	const char *xml;
	size_t xmllen;
	size_t avail;
	size_t send;
	bool   last;
	size_t bufsz;
	char *buf;

	if (strcmp(annex, "target.xml") != 0)
		goto notfound;

	xml = kvm_gdb__arch_target_xml();
	if (!xml)
		goto notfound;

	xmllen = strlen(xml);
	if (offset >= xmllen) {
		gdb_send_packet(fd, "l");	/* end-of-data */
		return true;
	}

	avail = xmllen - offset;
	send  = (avail < reqlen) ? avail : reqlen;
	last  = (offset + send >= xmllen);

	/* Response: 'm' (more) or 'l' (last) followed by data */
	bufsz = 2 + send * 2 + 1;
	buf = malloc(bufsz);
	if (!buf) {
		gdb_send_error(fd, ENOMEM);
		return true;
	}
	buf[0] = last ? 'l' : 'm';
	/* The content is text, not binary - copy it directly */
	memcpy(buf + 1, xml + offset, send);
	buf[1 + send] = '\0';
	gdb_send_packet(fd, buf);
	free(buf);
	return true;

notfound:
	gdb_send_packet(fd, "E00");
	return true;
}

/* ------------------------------------------------------------------ */
/* Main GDB packet dispatcher                                          */
/* ------------------------------------------------------------------ */

/*
 * Handle one GDB packet.
 * Returns:
 *   0  continue protocol loop
 *   1  resume guest (c / s / C / S)
 *   2  detach / kill
 */
static int handle_packet(int fd, const char *pkt, size_t pkt_len)
{
	const char *p = pkt;
	const char *pkt_end = pkt + pkt_len;

	switch (*p++) {

	/* ---- ? : stop reason ---- */
	case '?':
		gdb_send_stop_reply(fd, current_vcpu());
		break;

	/* ---- g : read all registers ---- */
	case 'g': {
		struct kvm_cpu *vcpu = current_vcpu();
		size_t regsz = kvm_gdb__arch_reg_pkt_size();
		size_t written = 0;
		u8 *regbuf;
		char *hexbuf;

		regbuf = malloc(regsz);
		if (!regbuf) { gdb_send_error(fd, ENOMEM); break; }

		kvm_gdb__arch_read_registers(vcpu, regbuf, &written);

		hexbuf = malloc(written * 2 + 1);
		if (!hexbuf) { free(regbuf); gdb_send_error(fd, ENOMEM); break; }
		bin_to_hex(regbuf, written, hexbuf);
		hexbuf[written * 2] = '\0';
		gdb_send_packet(fd, hexbuf);
		free(hexbuf);
		free(regbuf);
		break;
	}

	/* ---- G : write all registers ---- */
	case 'G': {
		struct kvm_cpu *vcpu = current_vcpu();
		size_t hexlen = strlen(p);
		size_t binlen = hexlen / 2;
		u8 *regbuf = malloc(binlen);
		if (!regbuf) { gdb_send_error(fd, ENOMEM); break; }
		if (hex_to_bin(p, hexlen, regbuf) < 0) {
			free(regbuf);
			gdb_send_error(fd, EINVAL);
			break;
		}
		kvm_gdb__arch_write_registers(vcpu, regbuf, binlen);
		free(regbuf);
		gdb_send_ok(fd);
		break;
	}

	/* ---- p n : read register n ---- */
	case 'p': {
		struct kvm_cpu *vcpu = current_vcpu();
		int regno = (int)parse_hex(&p);
		u8 regbuf[16] = {0};
		size_t rsize = 0;
		char hexbuf[33];

		if (kvm_gdb__arch_read_register(vcpu, regno, regbuf, &rsize) < 0) {
			gdb_send_error(fd, EINVAL);
			break;
		}
		bin_to_hex(regbuf, rsize, hexbuf);
		hexbuf[rsize * 2] = '\0';
		gdb_send_packet(fd, hexbuf);
		break;
	}

	/* ---- P n=v : write register n ---- */
	case 'P': {
		struct kvm_cpu *vcpu = current_vcpu();
		int regno = (int)parse_hex(&p);
		size_t hexlen;
		u8 regbuf[16] = {0};

		if (*p++ != '=') { gdb_send_error(fd, EINVAL); break; }
		hexlen = strlen(p);
		hex_to_bin(p, hexlen, regbuf);
		if (kvm_gdb__arch_write_register(vcpu, regno, regbuf,
						 hexlen / 2) < 0)
			gdb_send_error(fd, EINVAL);
		else
			gdb_send_ok(fd);
		break;
	}

	/* ---- m addr,len : read memory ---- */
	case 'm': {
		u64 addr = parse_hex(&p);
		u64 len;
		u8 *mem;
		char *hexbuf;

		if (*p++ != ',') { gdb_send_error(fd, EINVAL); break; }
		len  = parse_hex(&p);
		if (len > 4096) len = 4096;

		mem = malloc(len);
		if (!mem) { gdb_send_error(fd, ENOMEM); break; }
		if (!gdb_read_guest_mem(addr, mem, len)) {
			free(mem);
			gdb_send_error(fd, EFAULT);
			break;
		}
		hexbuf = malloc(len * 2 + 1);
		if (!hexbuf) { free(mem); gdb_send_error(fd, ENOMEM); break; }
		bin_to_hex(mem, len, hexbuf);
		hexbuf[len * 2] = '\0';
		gdb_send_packet(fd, hexbuf);
		free(hexbuf);
		free(mem);
		break;
	}

	/* ---- M addr,len:data : write memory ---- */
	case 'M': {
		u64 addr = parse_hex(&p);
		u64 len;
		u8 *mem;

		if (*p++ != ',') { gdb_send_error(fd, EINVAL); break; }
		len  = parse_hex(&p);
		if (*p++ != ':') { gdb_send_error(fd, EINVAL); break; }
		if (len > 4096) { gdb_send_error(fd, EINVAL); break; }
		if ((ptrdiff_t)(len * 2) > pkt_end - p) {
			gdb_send_error(fd, EINVAL);
			break;
		}

		mem = malloc(len);
		if (!mem) { gdb_send_error(fd, ENOMEM); break; }
		if (hex_to_bin(p, len * 2, mem) < 0 ||
		    !gdb_write_guest_mem(addr, mem, len)) {
			free(mem);
			gdb_send_error(fd, EFAULT);
			break;
		}
		free(mem);
		gdb_send_ok(fd);
		break;
	}

	/* ---- X addr,len:data : write binary memory ---- */
	case 'X': {
		u64 addr = parse_hex(&p);
		u64 len;
		const char *data;
		size_t data_len;
		u8 *mem;

		if (*p++ != ',') { gdb_send_error(fd, EINVAL); break; }
		len  = parse_hex(&p);
		if (*p++ != ':') { gdb_send_error(fd, EINVAL); break; }
		if (len == 0) {
			gdb_send_ok(fd);
			break;
		}
		if (len > 4096) { gdb_send_error(fd, EINVAL); break; }
		data = p;
		data_len = (size_t)(pkt_end - data);
		mem = malloc(len);
		if (!mem) { gdb_send_error(fd, ENOMEM); break; }
		if (gdb_unescape_binary(data, data_len, mem, len) < 0 ||
		    !gdb_write_guest_mem(addr, mem, len)) {
			free(mem);
			gdb_send_error(fd, EFAULT);
			break;
		}
		free(mem);
		gdb_send_ok(fd);
		break;
	}

	/* ---- c [addr] : continue ---- */
	case 'c': {
		if (*p) {
			u64 addr = parse_hex(&p);
			kvm_gdb__arch_set_pc(current_vcpu(), addr);
		}
		gdb.single_step = prepare_sw_bp_resume(true) ? true : false;
		return 1;	/* resume */
	}

	/* ---- C sig[;addr] : continue with signal ---- */
	case 'C': {
		/* We ignore the signal number but honour the address. */
		parse_hex(&p);	/* skip signal */
		if (*p == ';') {
			u64 addr;

			p++;
			addr = parse_hex(&p);
			kvm_gdb__arch_set_pc(current_vcpu(), addr);
		}
		gdb.single_step = prepare_sw_bp_resume(true) ? true : false;
		return 1;	/* resume */
	}

	/* ---- s [addr] : single step ---- */
	case 's': {
		if (*p) {
			u64 addr = parse_hex(&p);
			kvm_gdb__arch_set_pc(current_vcpu(), addr);
		}
		gdb.single_step = true;
		prepare_sw_bp_resume(false);
		return 1;	/* resume */
	}

	/* ---- S sig[;addr] : step with signal ---- */
	case 'S': {
		parse_hex(&p);	/* skip signal */
		if (*p == ';') {
			u64 addr;

			p++;
			addr = parse_hex(&p);
			kvm_gdb__arch_set_pc(current_vcpu(), addr);
		}
		gdb.single_step = true;
		prepare_sw_bp_resume(false);
		return 1;
	}

	/* ---- Z type,addr,len : insert breakpoint/watchpoint ---- */
	case 'Z': {
		int type = (int)parse_hex(&p);
		u64 addr;
		int len;
		int rc;

		if (*p++ != ',') { gdb_send_error(fd, EINVAL); break; }
		addr = parse_hex(&p);
		if (*p++ != ',') { gdb_send_error(fd, EINVAL); break; }
		len  = (int)parse_hex(&p);

		if (type == 0) {
			rc = sw_bp_insert(addr, len);
		} else {
			/* type 1=exec, 2=write, 3=read, 4=access */
			int hwtype = type - 1;	/* 0=exec,1=write,2=read,3=access */
				rc = hw_bp_insert(hwtype, addr, len);
				if (rc == 0)
					apply_debug_to_all(NULL, false);
		}
		if (rc == 0) gdb_send_ok(fd); else gdb_send_error(fd, ENOSPC);
		break;
	}

	/* ---- z type,addr,len : remove breakpoint/watchpoint ---- */
	case 'z': {
		int type = (int)parse_hex(&p);
		u64 addr;
		int len;
		int rc;

		if (*p++ != ',') { gdb_send_error(fd, EINVAL); break; }
		addr = parse_hex(&p);
		if (*p++ != ',') { gdb_send_error(fd, EINVAL); break; }
		len  = (int)parse_hex(&p);

		if (type == 0) {
			rc = sw_bp_remove(addr, len);
		} else {
			int hwtype = type - 1;
				rc = hw_bp_remove(hwtype, addr, len);
				if (rc == 0)
					apply_debug_to_all(NULL, false);
		}
		if (rc == 0) gdb_send_ok(fd); else gdb_send_error(fd, ENOENT);
		break;
	}

	/* ---- H op tid : set thread ---- */
	case 'H': {
		char op = *p++;
		int vcpu_idx = tid_to_vcpu(p);
		if (vcpu_idx >= gdb.kvm->nrcpus || vcpu_idx < -1) {
			gdb_send_error(fd, EINVAL);
			break;
		}
		if (op == 'g')
			gdb.g_tid = (vcpu_idx < 0) ? -1 : vcpu_idx + 1;
		else if (op == 'c')
			gdb.c_tid = (vcpu_idx < 0) ? -1 : vcpu_idx + 1;
		else {
			gdb_send_error(fd, EINVAL);
			break;
		}
		gdb_send_ok(fd);
		break;
	}

	/* ---- T tid : is thread alive? ---- */
	case 'T': {
		u64 tid = parse_hex(&p);
		int idx = (int)(tid - 1);
		if (tid > 0 && idx < gdb.kvm->nrcpus)
			gdb_send_ok(fd);
		else
			gdb_send_error(fd, ESRCH);
		break;
	}

	/* ---- D : detach ---- */
	case 'D':
		gdb_send_ok(fd);
		return 2;

	/* ---- k : kill ---- */
	case 'k':
		return 2;

	/* ---- q : general queries ---- */
	case 'q': {
		if (strncmp(p, "Supported", 9) == 0) {
			char buf[256];
			snprintf(buf, sizeof(buf),
				 "PacketSize=%x;"
				 "qXfer:features:read+;"
				 "swbreak+;hwbreak+",
				 GDB_PACKET_MAX);
			gdb_send_packet(fd, buf);

		} else if (strncmp(p, "Xfer:features:read:", 19) == 0) {
			char annex[64];
			const char *colon;
			size_t annex_len;
			u64 offset;
			u64 reqlen;

			p += 19;
			/* annex:offset,length */
			colon = strchr(p, ':');
			if (!colon) { gdb_send_error(fd, EINVAL); break; }
			annex_len = (size_t)(colon - p);
			if (annex_len >= sizeof(annex)) annex_len = sizeof(annex)-1;
			memcpy(annex, p, annex_len);
			annex[annex_len] = '\0';
			p = colon + 1;
			offset = parse_hex(&p);
			if (*p++ != ',') { gdb_send_error(fd, EINVAL); break; }
			reqlen = parse_hex(&p);
			handle_qxfer_features(fd, annex, offset, reqlen);

		} else if (strcmp(p, "C") == 0) {
			/* Current thread ID */
			char buf[32];
			int tid = gdb.stopped_vcpu
				  ? (int)(gdb.stopped_vcpu->cpu_id + 1) : 1;
			snprintf(buf, sizeof(buf), "QC%x", tid);
			gdb_send_packet(fd, buf);

		} else if (strcmp(p, "fThreadInfo") == 0) {
			/* First batch of thread IDs */
			char buf[256];
			char *bp = buf;
			int i;

			*bp++ = 'm';
			for (i = 0; i < gdb.kvm->nrcpus; i++) {
				size_t rem = sizeof(buf) - (size_t)(bp - buf);
				int w = snprintf(bp, rem, "%s%x", i ? "," : "", i + 1);
				if (w < 0)
					break;
				if ((size_t)w >= rem) {
					bp = buf + sizeof(buf) - 1;
					break;
				}
				bp += w;
			}
			*bp = '\0';
			gdb_send_packet(fd, buf);

		} else if (strcmp(p, "sThreadInfo") == 0) {
			gdb_send_packet(fd, "l");	/* end of thread list */

		} else if (strncmp(p, "ThreadExtraInfo,", 16) == 0) {
			u64 tid;
			int idx;
			char info[64];
			char hexinfo[sizeof(info) * 2 + 1];

			p += 16;
			tid = parse_hex(&p);
			idx = (int)(tid - 1);
			if (idx >= 0 && idx < gdb.kvm->nrcpus)
				snprintf(info, sizeof(info),
					 "vCPU %d", idx);
			else
				snprintf(info, sizeof(info), "unknown");
			bin_to_hex(info, strlen(info), hexinfo);
			hexinfo[strlen(info) * 2] = '\0';
			gdb_send_packet(fd, hexinfo);

		} else if (strncmp(p, "Symbol:", 7) == 0) {
			gdb_send_ok(fd);
		} else {
			gdb_send_empty(fd);
		}
		break;
	}

	/* ---- v : extended commands ---- */
	case 'v': {
		if (strncmp(p, "Cont?", 5) == 0) {
			gdb_send_empty(fd);
		} else if (strncmp(p, "Cont;", 5) == 0) {
			gdb_send_empty(fd);
		} else {
			gdb_send_empty(fd);
		}
		break;
	}

	default:
		gdb_send_empty(fd);
		break;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Debug session: handle GDB interaction when guest is stopped        */
/* ------------------------------------------------------------------ */

/*
 * Called from the GDB thread when a vCPU has stopped.
 * Loops handling GDB packets until a resume command is received.
 *
 * send_stop_first: if true, send a T05 stop reply immediately.
 *   - true:  use when resuming from c/s (GDB is waiting for a stop reply)
 *            or after Ctrl+C (GDB expects a stop reply after 0x03).
 *   - false: use for the initial GDB connection handshake (GDB will ask
 *            for the stop reason via '?').
 *
 * Returns:
 *   0  resume normally
 *   1  detach / kill
 */
static int run_debug_session(struct kvm_cpu *vcpu, bool send_stop_first)
{
	int fd = gdb.fd;
	int ret = 0;
	int r;
	int action;
	char *pkt = malloc(GDB_PACKET_MAX);

	if (!pkt)
		return 1;

	/* Announce the stop only when the caller needs it */
	if (send_stop_first)
		gdb_send_stop_reply(fd, vcpu);

	while (1) {
		/*
		 * Poll for: socket data or Ctrl+C while running.
		 * Here the guest is stopped so just do a blocking read.
		 */
		r = gdb_recv_packet(fd, pkt, GDB_PACKET_MAX);
		if (r == -1) {
			pr_warning("GDB: connection lost");
			ret = 1;
			break;
		}
		if (r == -2) {
			/* Ctrl+C while stopped - send stop reply again */
			gdb_send_stop_reply(fd, vcpu);
			continue;
		}

		action = handle_packet(fd, pkt, (size_t)r);
		if (action == 1)
			break;	/* resume */
		if (action == 2) {
			ret = 1;
			break;	/* detach/kill */
		}
	}

	free(pkt);
	return ret;
}

/* ------------------------------------------------------------------ */
/* GDB thread: accept connection and handle Ctrl+C                    */
/* ------------------------------------------------------------------ */

/*
 * Enable debug interception on all vCPUs after GDB connects.
 */
static void gdb_enable_debug(void)
{
	int i;

	for (i = 0; i < gdb.kvm->nrcpus; i++)
		kvm_gdb__arch_set_debug(gdb.kvm->cpus[i], false, gdb.hw_bp);
}

/*
 * Disable debug interception on all vCPUs when GDB disconnects.
 */
static void gdb_disable_debug(void)
{
	struct kvm_guest_debug dbg = { 0 };	/* control = 0: disable all */
	int i;

	for (i = 0; i < gdb.kvm->nrcpus; i++) {
		if (ioctl(gdb.kvm->cpus[i]->vcpu_fd,
			  KVM_SET_GUEST_DEBUG, &dbg) < 0)
			pr_warning("GDB: KVM_SET_GUEST_DEBUG(disable) failed: %s",
				   strerror(errno));
	}
}

/*
 * Main body of the GDB thread.
 * Accepts one GDB connection at a time, handles debug sessions.
 */
static void *gdb_thread_fn(void *arg)
{
	struct kvm *kvm = arg;

	/* Block signals that are intended for vCPU threads */
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGKVMEXIT);
	sigaddset(&mask, SIGKVMPAUSE);
	sigaddset(&mask, SIGKVMTASK);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

	pr_info("GDB: listening on port %d", gdb.port);

	while (1) {
		/* Accept a new GDB connection */
		struct sockaddr_in client;
		socklen_t clen = sizeof(client);
		int cfd;
		int one;
		int i;

		cfd = accept(gdb.listen_fd, (struct sockaddr *)&client, &clen);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			pr_warning("GDB: accept failed: %s", strerror(errno));
			break;
		}

		/* Disable Nagle for lower latency */
		one = 1;
		setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

		pr_info("GDB: connected from %s", inet_ntoa(client.sin_addr));

		if (gdb.wait) {
			/*
			 * --gdb-wait mode: vCPUs have not yet called KVM_RUN.
			 * Enable single-step on vCPU 0 so it stops at its
			 * very first instruction.  All other vCPUs get normal
			 * debug (SW_BP intercept) without single-step.
			 *
			 * This must be done BEFORE signalling connected_cond so
			 * that kvm_gdb__init() cannot return (and the vCPU
			 * threads cannot start) until the debug flags are set.
			 */
			kvm_gdb__arch_set_debug(kvm->cpus[0], true, gdb.hw_bp);
			for (i = 1; i < kvm->nrcpus; i++)
				kvm_gdb__arch_set_debug(kvm->cpus[i], false,
							gdb.hw_bp);
		}

		pthread_mutex_lock(&gdb.lock);
		gdb.fd        = cfd;
		gdb.connected = true;
		/* Notify the main thread if it was waiting for --gdb-wait */
		pthread_cond_broadcast(&gdb.connected_cond);
		pthread_mutex_unlock(&gdb.lock);

		if (!gdb.wait) {
			/*
			 * Normal (non-wait) mode: the guest is already running.
			 *
			 * Pause all vCPUs FIRST, then enable debug interception.
			 * This prevents any INT3 in the running guest (e.g. from
			 * Linux jump-label patching) from triggering
			 * KVM_EXIT_DEBUG before GDB has finished its initial
			 * handshake.
			 *
			 * The initial debug session runs WITHOUT sending a stop
			 * reply upfront; GDB will ask for the stop reason with
			 * the '?' packet once it has completed the handshake.
			 */
			kvm__pause(kvm);
			gdb_enable_debug();

			if (run_debug_session(kvm->cpus[0], false)) {
				/* GDB detached or connection lost */
				gdb_disable_debug();
				sw_bp_remove_all();
				kvm__continue(kvm);
				goto disconnect;
			}

			/* GDB sent c/s - apply debug flags and resume */
			apply_debug_to_all(gdb.single_step ? kvm->cpus[0] : NULL,
					   gdb.single_step);
			kvm__continue(kvm);

		} else {
			/*
			 * --gdb-wait mode: wait for vCPU 0 to stop at its
			 * first instruction (via the single-step flag we set
			 * above).
			 */
			struct kvm_cpu *vcpu;

			pthread_mutex_lock(&gdb.lock);
			while (!gdb.stopped_vcpu)
				pthread_cond_wait(&gdb.vcpu_stopped, &gdb.lock);
			vcpu = gdb.stopped_vcpu;
			pthread_mutex_unlock(&gdb.lock);

			/* Pause all other vCPUs */
			kvm__pause(kvm);

			/*
			 * Initial session: no upfront stop reply.
			 * GDB will ask with '?' after completing its handshake.
			 */
			if (run_debug_session(vcpu, false)) {
				pthread_mutex_lock(&gdb.lock);
				gdb.stopped_vcpu = NULL;
				pthread_cond_signal(&gdb.vcpu_resume);
				pthread_mutex_unlock(&gdb.lock);
				kvm__continue(kvm);
				goto disconnect;
			}

			apply_debug_to_all(gdb.single_step ? vcpu : NULL,
					   gdb.single_step);
			pthread_mutex_lock(&gdb.lock);
			gdb.stopped_vcpu = NULL;
			pthread_cond_signal(&gdb.vcpu_resume);
			pthread_mutex_unlock(&gdb.lock);
			kvm__continue(kvm);
		}

		/* -------------------------------------------------------- */
		/* Main event loop: guest is now running                    */
		/* -------------------------------------------------------- */
		while (1) {
			struct kvm_cpu *vcpu;

			pthread_mutex_lock(&gdb.lock);
			vcpu = gdb.stopped_vcpu;
			pthread_mutex_unlock(&gdb.lock);

			if (vcpu) {
				bool auto_resume;

				/*
				 * A vCPU stopped at a breakpoint or single-step.
				 * Pause all other vCPUs (stopped_vcpu already has
				 * paused=1, so kvm__pause() counts it immediately).
				 *
				 * Send T05 proactively - GDB is waiting for a stop
				 * reply after the 'c'/'s' command it sent.
				 */
				kvm__pause(kvm);
				kvm_gdb__arch_handle_stop(vcpu);

				if (finish_sw_bp_resume(&auto_resume)) {
					gdb.single_step = false;
					kvm_gdb__arch_prepare_resume(vcpu, false, true);
					pthread_mutex_lock(&gdb.lock);
					gdb.stopped_vcpu = NULL;
					pthread_cond_signal(&gdb.vcpu_resume);
					pthread_mutex_unlock(&gdb.lock);

					if (auto_resume) {
						apply_debug_to_all(NULL, false);
						kvm__continue(kvm);
						continue;
					}
				}

				if (run_debug_session(vcpu, true)) {
					pthread_mutex_lock(&gdb.lock);
					gdb.stopped_vcpu = NULL;
					pthread_cond_signal(&gdb.vcpu_resume);
					pthread_mutex_unlock(&gdb.lock);
					kvm__continue(kvm);
					goto disconnect;
				}

				kvm_gdb__arch_prepare_resume(vcpu, gdb.single_step, true);
				apply_debug_to_all(gdb.single_step ? vcpu : NULL,
					   gdb.single_step);
				pthread_mutex_lock(&gdb.lock);
				gdb.stopped_vcpu = NULL;
				pthread_cond_signal(&gdb.vcpu_resume);
				pthread_mutex_unlock(&gdb.lock);
				kvm__continue(kvm);

			} else {
				/*
				 * No vCPU stopped. Poll the socket for Ctrl+C
				 * or unexpected packets.
				 */
				struct pollfd pfd = {
					.fd     = cfd,
					.events = POLLIN,
				};
				int r;
				unsigned char byte;
				ssize_t n;

				r = poll(&pfd, 1, 200 /* ms */);
				if (r < 0 && errno != EINTR)
					goto disconnect;
				if (r == 0)
					continue;

				/* Peek at the first byte */
				n = recv(cfd, &byte, 1, MSG_PEEK);
				if (n <= 0)
					goto disconnect;

				if (byte == 0x03) {
					struct kvm_cpu *cur;

					recv(cfd, &byte, 1, 0);	/* consume */

					/*
					 * Ctrl+C: pause all vCPUs.
					 * If a vCPU happened to stop at a
					 * breakpoint at the same time, use that
					 * one; otherwise use vCPU 0.
					 */
					kvm__pause(kvm);

					pthread_mutex_lock(&gdb.lock);
					cur = gdb.stopped_vcpu
					      ? gdb.stopped_vcpu
					      : kvm->cpus[0];
					pthread_mutex_unlock(&gdb.lock);

					/*
					 * Send T05 proactively - GDB expects a
					 * stop reply after the Ctrl+C it sent.
					 */
					if (run_debug_session(cur, true)) {
						pthread_mutex_lock(&gdb.lock);
						if (gdb.stopped_vcpu) {
							gdb.stopped_vcpu = NULL;
							pthread_cond_signal(
							  &gdb.vcpu_resume);
						}
						pthread_mutex_unlock(&gdb.lock);
						kvm__continue(kvm);
						goto disconnect;
					}

					kvm_gdb__arch_prepare_resume(cur, gdb.single_step,
							   !!gdb.stopped_vcpu);
					apply_debug_to_all(gdb.single_step ? cur : NULL,
						   gdb.single_step);

					pthread_mutex_lock(&gdb.lock);
					if (gdb.stopped_vcpu) {
						gdb.stopped_vcpu = NULL;
						pthread_cond_signal(
						  &gdb.vcpu_resume);
					}
					pthread_mutex_unlock(&gdb.lock);

					kvm__continue(kvm);

				} else {
					/*
					 * Unexpected packet while running -
					 * handle it (probably a query).
					 */
					char pktbuf[GDB_PACKET_MAX];
					int pr;

					pr = gdb_recv_packet(cfd, pktbuf,
							sizeof(pktbuf));
					if (pr < 0)
						goto disconnect;
					handle_packet(cfd, pktbuf, (size_t)pr);
				}
			}
		}

disconnect:
		pr_info("GDB: client disconnected");
		gdb_disable_debug();
		sw_bp_remove_all();

		pthread_mutex_lock(&gdb.lock);
		gdb.fd        = -1;
		gdb.connected = false;
		/* If a vCPU is still stuck waiting, let it go */
		if (gdb.stopped_vcpu) {
			gdb.stopped_vcpu->paused = 0;
			gdb.stopped_vcpu = NULL;
			pthread_cond_broadcast(&gdb.vcpu_resume);
		}
		pthread_mutex_unlock(&gdb.lock);

		close(cfd);
	}

	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * Called from a vCPU thread when KVM_EXIT_DEBUG is received.
 * Blocks until the GDB session says to resume.
 */
void kvm_gdb__handle_debug(struct kvm_cpu *vcpu)
{
	if (!gdb.active)
		return;

	/*
	 * Filter out native guest INT3s that are NOT in our sw_bp table.
	 *
	 * With KVM_GUESTDBG_USE_SW_BP enabled, KVM intercepts every INT3
	 * in the guest, including ones that belong to the guest kernel
	 * itself (e.g. int3_selftest(), jump-label patching, kprobes).
	 * Those are not our breakpoints, so we re-inject the #BP exception
	 * back to the guest and return without involving GDB at all.
	 *
	 * This check is intentionally done before acquiring gdb.lock so
	 * that the common fast-path (native guest INT3, not our BP) does
	 * not serialise on the lock.
	 */
	if (kvm_gdb__arch_is_sw_bp_exit(vcpu)) {
		u64 bp_addr = kvm_gdb__arch_debug_pc(vcpu);
		pr_warning("GDB: sw_bp exit at 0x%llx, active=%d",
			   (unsigned long long)bp_addr,
			   sw_bp_active_at(bp_addr));
		if (!sw_bp_active_at(bp_addr)) {
			kvm_gdb__arch_reinject_sw_bp(vcpu);
			return;
		}
	}

	pthread_mutex_lock(&gdb.lock);

	if (!gdb.connected) {
		/* GDB not connected yet - ignore debug events */
		pthread_mutex_unlock(&gdb.lock);
		return;
	}

	/*
	 * Mark ourselves as paused so that kvm__pause() from the GDB
	 * thread does not wait for us (it counts paused vCPUs immediately).
	 */
	vcpu->paused       = 1;
	gdb.stopped_vcpu   = vcpu;

	/* Wake the GDB thread */
	pthread_cond_signal(&gdb.vcpu_stopped);

	/* Sleep until the GDB thread says we may run again */
	pthread_cond_wait(&gdb.vcpu_resume, &gdb.lock);

	vcpu->paused = 0;
	pthread_mutex_unlock(&gdb.lock);
}

bool kvm_gdb__active(struct kvm *kvm)
{
	return gdb.active;
}

/* ------------------------------------------------------------------ */
/* init / exit                                                         */
/* ------------------------------------------------------------------ */

int kvm_gdb__init(struct kvm *kvm)
{
	int reuse = 1;
	struct sockaddr_in addr;

	if (!kvm->cfg.gdb_port)
		return 0;

#if !defined(CONFIG_X86) && !defined(CONFIG_ARM64)
	pr_err("GDB stub is supported only on x86 and arm64");
	return -ENOSYS;
#endif

	gdb.port = kvm->cfg.gdb_port;
	gdb.wait = kvm->cfg.gdb_wait;
	gdb.kvm  = kvm;

	if (kvm->nrcpus > 1)
		pr_warning("GDB: SMP guest debugging may make 'next/finish' unstable; use -c 1 for reliable stepping");

	/* Create TCP listen socket */
	gdb.listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (gdb.listen_fd < 0)
		die_perror("GDB: socket");

	setsockopt(gdb.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
		   sizeof(reuse));

	addr.sin_family      = AF_INET;
	addr.sin_port        = htons((u16)gdb.port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(gdb.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die_perror("GDB: bind");
	if (listen(gdb.listen_fd, 1) < 0)
		die_perror("GDB: listen");

	gdb.active = true;

	if (pthread_create(&gdb.thread, NULL, gdb_thread_fn, kvm) != 0)
		die_perror("GDB: pthread_create");

	if (gdb.wait) {
		pr_info("GDB: waiting for connection on port %d ...",
			gdb.port);
		pthread_mutex_lock(&gdb.lock);
		while (!gdb.connected)
			pthread_cond_wait(&gdb.connected_cond, &gdb.lock);
		pthread_mutex_unlock(&gdb.lock);
		pr_info("GDB: client connected, starting VM");
	}

	return 0;
}
late_init(kvm_gdb__init);

int kvm_gdb__exit(struct kvm *kvm)
{
	if (!gdb.active)
		return 0;

	gdb.active = false;

	/*
	 * Unblock the GDB thread if it is waiting in accept().
	 *
	 * close() alone is NOT sufficient on Linux: close() removes the fd
	 * from the process fd table but the underlying socket object lives on
	 * (accept() holds an internal reference), so accept() keeps blocking.
	 * shutdown(SHUT_RDWR) triggers the socket's wait-queue wakeup, which
	 * causes accept() to return immediately with EINVAL.
	 */
	if (gdb.listen_fd >= 0) {
		shutdown(gdb.listen_fd, SHUT_RDWR);
		close(gdb.listen_fd);
		gdb.listen_fd = -1;
	}

	/* Unblock the GDB thread if it is inside a debug session */
	if (gdb.fd >= 0) {
		close(gdb.fd);
		gdb.fd = -1;
	}

	/* Wake any vCPU stuck in kvm_gdb__handle_debug() */
	pthread_mutex_lock(&gdb.lock);
	if (gdb.stopped_vcpu) {
		gdb.stopped_vcpu->paused = 0;
		gdb.stopped_vcpu         = NULL;
	}
	pthread_cond_broadcast(&gdb.vcpu_resume);
	pthread_mutex_unlock(&gdb.lock);

	pthread_join(gdb.thread, NULL);
	return 0;
}
late_exit(kvm_gdb__exit);
