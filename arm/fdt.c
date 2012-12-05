#include "kvm/devices.h"
#include "kvm/fdt.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/virtio-mmio.h"

#include "arm-common/gic.h"

#include <stdbool.h>

#include <asm/setup.h>
#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/sizes.h>

#define DEBUG			0
#define DEBUG_FDT_DUMP_FILE	"/tmp/kvmtool.dtb"

static char kern_cmdline[COMMAND_LINE_SIZE];

bool kvm__load_firmware(struct kvm *kvm, const char *firmware_filename)
{
	return false;
}

int kvm__arch_setup_firmware(struct kvm *kvm)
{
	return 0;
}

#if DEBUG
static void dump_fdt(void *fdt)
{
	int count, fd;

	fd = open(DEBUG_FDT_DUMP_FILE, O_CREAT | O_TRUNC | O_RDWR, 0666);
	if (fd < 0)
		die("Failed to write dtb to %s", DEBUG_FDT_DUMP_FILE);

	count = write(fd, fdt, FDT_MAX_SIZE);
	if (count < 0)
		die_perror("Failed to dump dtb");

	pr_info("Wrote %d bytes to dtb %s\n", count, DEBUG_FDT_DUMP_FILE);
	close(fd);
}
#else
static void dump_fdt(void *fdt) { }
#endif

#define DEVICE_NAME_MAX_LEN 32
static void generate_virtio_mmio_node(void *fdt, struct virtio_mmio *vmmio)
{
	char dev_name[DEVICE_NAME_MAX_LEN];
	u64 addr = vmmio->addr;
	u64 reg_prop[] = {
		cpu_to_fdt64(addr),
		cpu_to_fdt64(VIRTIO_MMIO_IO_SIZE)
	};
	u32 irq_prop[] = {
		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_SPI),
		cpu_to_fdt32(vmmio->irq - GIC_SPI_IRQ_BASE),
		cpu_to_fdt32(GIC_FDT_IRQ_FLAGS_EDGE_LO_HI),
	};

	snprintf(dev_name, DEVICE_NAME_MAX_LEN, "virtio@%llx", addr);

	_FDT(fdt_begin_node(fdt, dev_name));
	_FDT(fdt_property_string(fdt, "compatible", "virtio,mmio"));
	_FDT(fdt_property(fdt, "reg", reg_prop, sizeof(reg_prop)));
	_FDT(fdt_property(fdt, "interrupts", irq_prop, sizeof(irq_prop)));
	_FDT(fdt_end_node(fdt));
}

static int setup_fdt(struct kvm *kvm)
{
	struct device_header *dev_hdr;
	u8 staging_fdt[FDT_MAX_SIZE];
	u32 gic_phandle		= fdt__alloc_phandle();
	u64 mem_reg_prop[]	= {
		cpu_to_fdt64(kvm->arch.memory_guest_start),
		cpu_to_fdt64(kvm->ram_size),
	};
	void *fdt		= staging_fdt;
	void *fdt_dest		= guest_flat_to_host(kvm,
						     kvm->arch.dtb_guest_start);
	void (*generate_cpu_nodes)(void *, struct kvm *, u32)
				= kvm->cpus[0]->generate_fdt_nodes;

	/* Create new tree without a reserve map */
	_FDT(fdt_create(fdt, FDT_MAX_SIZE));
	if (kvm->nrcpus > 1)
		_FDT(fdt_add_reservemap_entry(fdt,
					      kvm->arch.smp_pen_guest_start,
					      ARM_SMP_PEN_SIZE));
	_FDT(fdt_finish_reservemap(fdt));

	/* Header */
	_FDT(fdt_begin_node(fdt, ""));
	_FDT(fdt_property_cell(fdt, "interrupt-parent", gic_phandle));
	_FDT(fdt_property_string(fdt, "compatible", "linux,dummy-virt"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x2));

	/* /chosen */
	_FDT(fdt_begin_node(fdt, "chosen"));
	_FDT(fdt_property_string(fdt, "bootargs", kern_cmdline));

	/* Initrd */
	if (kvm->arch.initrd_size != 0) {
		u32 ird_st_prop = cpu_to_fdt64(kvm->arch.initrd_guest_start);
		u32 ird_end_prop = cpu_to_fdt64(kvm->arch.initrd_guest_start +
					       kvm->arch.initrd_size);

		_FDT(fdt_property(fdt, "linux,initrd-start",
				   &ird_st_prop, sizeof(ird_st_prop)));
		_FDT(fdt_property(fdt, "linux,initrd-end",
				   &ird_end_prop, sizeof(ird_end_prop)));
	}
	_FDT(fdt_end_node(fdt));

	/* Memory */
	_FDT(fdt_begin_node(fdt, "memory"));
	_FDT(fdt_property_string(fdt, "device_type", "memory"));
	_FDT(fdt_property(fdt, "reg", mem_reg_prop, sizeof(mem_reg_prop)));
	_FDT(fdt_end_node(fdt));

	/* CPU and peripherals (interrupt controller, timers, etc) */
	if (generate_cpu_nodes)
		generate_cpu_nodes(fdt, kvm, gic_phandle);

	/* Virtio MMIO devices */
	dev_hdr = device__first_dev(DEVICE_BUS_MMIO);
	while (dev_hdr) {
		generate_virtio_mmio_node(fdt, dev_hdr->data);
		dev_hdr = device__next_dev(dev_hdr);
	}

	/* Finalise. */
	_FDT(fdt_end_node(fdt));
	_FDT(fdt_finish(fdt));

	_FDT(fdt_open_into(fdt, fdt_dest, FDT_MAX_SIZE));
	_FDT(fdt_pack(fdt_dest));

	dump_fdt(fdt_dest);
	return 0;
}
late_init(setup_fdt);

static int read_image(int fd, void **pos, void *limit)
{
	int count;

	while (((count = xread(fd, *pos, SZ_64K)) > 0) && *pos <= limit)
		*pos += count;

	if (pos < 0)
		die_perror("xread");

	return *pos < limit ? 0 : -ENOMEM;
}

#define FDT_ALIGN	SZ_2M
#define INITRD_ALIGN	4
#define SMP_PEN_ALIGN	PAGE_SIZE
int load_flat_binary(struct kvm *kvm, int fd_kernel, int fd_initrd,
		     const char *kernel_cmdline)
{
	void *pos, *kernel_end, *limit;
	unsigned long guest_addr;

	if (lseek(fd_kernel, 0, SEEK_SET) < 0)
		die_perror("lseek");

	/*
	 * Linux requires the initrd, pen and dtb to be mapped inside
	 * lowmem, so we can't just place them at the top of memory.
	 */
	limit = kvm->ram_start + min(kvm->ram_size, (u64)SZ_256M) - 1;

	pos = kvm->ram_start + ARM_KERN_OFFSET;
	kvm->arch.kern_guest_start = host_to_guest_flat(kvm, pos);
	if (read_image(fd_kernel, &pos, limit) == -ENOMEM)
		die("kernel image too big to contain in guest memory.");

	kernel_end = pos;
	pr_info("Loaded kernel to 0x%llx (%llu bytes)",
		kvm->arch.kern_guest_start,
		host_to_guest_flat(kvm, pos) - kvm->arch.kern_guest_start);

	/*
	 * Now load backwards from the end of memory so the kernel
	 * decompressor has plenty of space to work with. First up is
	 * the SMP pen if we have more than one virtual CPU...
	 */
	pos = limit;
	if (kvm->cfg.nrcpus > 1) {
		pos -= (ARM_SMP_PEN_SIZE + SMP_PEN_ALIGN);
		guest_addr = ALIGN(host_to_guest_flat(kvm, pos), SMP_PEN_ALIGN);
		pos = guest_flat_to_host(kvm, guest_addr);
		if (pos < kernel_end)
			die("SMP pen overlaps with kernel image.");

		kvm->arch.smp_pen_guest_start = guest_addr;
		pr_info("Placing SMP pen at 0x%llx - 0x%llx",
			kvm->arch.smp_pen_guest_start,
			host_to_guest_flat(kvm, limit));
		limit = pos;
	}

	/* ...now the device tree blob... */
	pos -= (FDT_MAX_SIZE + FDT_ALIGN);
	guest_addr = ALIGN(host_to_guest_flat(kvm, pos), FDT_ALIGN);
	pos = guest_flat_to_host(kvm, guest_addr);
	if (pos < kernel_end)
		die("fdt overlaps with kernel image.");

	kvm->arch.dtb_guest_start = guest_addr;
	pr_info("Placing fdt at 0x%llx - 0x%llx",
		kvm->arch.dtb_guest_start,
		host_to_guest_flat(kvm, limit));
	limit = pos;

	/* ... and finally the initrd, if we have one. */
	if (fd_initrd != -1) {
		struct stat sb;
		unsigned long initrd_start;

		if (lseek(fd_initrd, 0, SEEK_SET) < 0)
			die_perror("lseek");

		if (fstat(fd_initrd, &sb))
			die_perror("fstat");

		pos -= (sb.st_size + INITRD_ALIGN);
		guest_addr = ALIGN(host_to_guest_flat(kvm, pos), INITRD_ALIGN);
		pos = guest_flat_to_host(kvm, guest_addr);
		if (pos < kernel_end)
			die("initrd overlaps with kernel image.");

		initrd_start = guest_addr;
		if (read_image(fd_initrd, &pos, limit) == -ENOMEM)
			die("initrd too big to contain in guest memory.");

		kvm->arch.initrd_guest_start = initrd_start;
		kvm->arch.initrd_size = host_to_guest_flat(kvm, pos) - initrd_start;
		pr_info("Loaded initrd to 0x%llx (%llu bytes)",
			kvm->arch.initrd_guest_start,
			kvm->arch.initrd_size);
	} else {
		kvm->arch.initrd_size = 0;
	}

	strncpy(kern_cmdline, kernel_cmdline, COMMAND_LINE_SIZE);
	kern_cmdline[COMMAND_LINE_SIZE - 1] = '\0';

	return true;
}

bool load_bzimage(struct kvm *kvm, int fd_kernel, int fd_initrd,
		  const char *kernel_cmdline)
{
	/* To b or not to b? That is the zImage. */
	return false;
}
