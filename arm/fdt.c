#include "kvm/devices.h"
#include "kvm/fdt.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/virtio-mmio.h"

#include "arm-common/gic.h"
#include "arm-common/pci.h"

#include <stdbool.h>

#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/psci.h>

static void dump_fdt(const char *dtb_file, void *fdt)
{
	int count, fd;

	fd = open(dtb_file, O_CREAT | O_TRUNC | O_RDWR, 0666);
	if (fd < 0)
		die("Failed to write dtb to %s", dtb_file);

	count = write(fd, fdt, FDT_MAX_SIZE);
	if (count < 0)
		die_perror("Failed to dump dtb");

	pr_debug("Wrote %d bytes to dtb %s", count, dtb_file);
	close(fd);
}

#define CPU_NAME_MAX_LEN 15
static void generate_cpu_nodes(void *fdt, struct kvm *kvm)
{
	int cpu;

	_FDT(fdt_begin_node(fdt, "cpus"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x1));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x0));

	for (cpu = 0; cpu < kvm->nrcpus; ++cpu) {
		char cpu_name[CPU_NAME_MAX_LEN];
		struct kvm_cpu *vcpu = kvm->cpus[cpu];
		unsigned long mpidr = kvm_cpu__get_vcpu_mpidr(vcpu);

		mpidr &= ARM_MPIDR_HWID_BITMASK;
		snprintf(cpu_name, CPU_NAME_MAX_LEN, "cpu@%lx", mpidr);

		_FDT(fdt_begin_node(fdt, cpu_name));
		_FDT(fdt_property_string(fdt, "device_type", "cpu"));
		_FDT(fdt_property_string(fdt, "compatible", vcpu->cpu_compatible));

		if (kvm->nrcpus > 1)
			_FDT(fdt_property_string(fdt, "enable-method", "psci"));

		_FDT(fdt_property_cell(fdt, "reg", mpidr));
		_FDT(fdt_end_node(fdt));
	}

	_FDT(fdt_end_node(fdt));
}

static void generate_irq_prop(void *fdt, u8 irq, enum irq_type irq_type)
{
	u32 irq_prop[] = {
		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_SPI),
		cpu_to_fdt32(irq - GIC_SPI_IRQ_BASE),
		cpu_to_fdt32(irq_type)
	};

	_FDT(fdt_property(fdt, "interrupts", irq_prop, sizeof(irq_prop)));
}

struct psci_fns {
	u32 cpu_suspend;
	u32 cpu_off;
	u32 cpu_on;
	u32 migrate;
};

static struct psci_fns psci_0_1_fns = {
	.cpu_suspend = KVM_PSCI_FN_CPU_SUSPEND,
	.cpu_off = KVM_PSCI_FN_CPU_OFF,
	.cpu_on = KVM_PSCI_FN_CPU_ON,
	.migrate = KVM_PSCI_FN_MIGRATE,
};

static struct psci_fns psci_0_2_aarch32_fns = {
	.cpu_suspend = PSCI_0_2_FN_CPU_SUSPEND,
	.cpu_off = PSCI_0_2_FN_CPU_OFF,
	.cpu_on = PSCI_0_2_FN_CPU_ON,
	.migrate = PSCI_0_2_FN_MIGRATE,
};

static struct psci_fns psci_0_2_aarch64_fns = {
	.cpu_suspend = PSCI_0_2_FN64_CPU_SUSPEND,
	.cpu_off = PSCI_0_2_FN_CPU_OFF,
	.cpu_on = PSCI_0_2_FN64_CPU_ON,
	.migrate = PSCI_0_2_FN64_MIGRATE,
};

static int setup_fdt(struct kvm *kvm)
{
	struct device_header *dev_hdr;
	u8 staging_fdt[FDT_MAX_SIZE];
	u64 mem_reg_prop[]	= {
		cpu_to_fdt64(kvm->arch.memory_guest_start),
		cpu_to_fdt64(kvm->ram_size),
	};
	struct psci_fns *fns;
	void *fdt		= staging_fdt;
	void *fdt_dest		= guest_flat_to_host(kvm,
						     kvm->arch.dtb_guest_start);
	void (*generate_mmio_fdt_nodes)(void *, struct device_header *,
					void (*)(void *, u8, enum irq_type));
	void (*generate_cpu_peripheral_fdt_nodes)(void *, struct kvm *)
					= kvm->cpus[0]->generate_fdt_nodes;

	/* Create new tree without a reserve map */
	_FDT(fdt_create(fdt, FDT_MAX_SIZE));
	_FDT(fdt_finish_reservemap(fdt));

	/* Header */
	_FDT(fdt_begin_node(fdt, ""));
	_FDT(fdt_property_cell(fdt, "interrupt-parent", PHANDLE_GIC));
	_FDT(fdt_property_string(fdt, "compatible", "linux,dummy-virt"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x2));

	/* /chosen */
	_FDT(fdt_begin_node(fdt, "chosen"));

	/* Pass on our amended command line to a Linux kernel only. */
	if (kvm->cfg.firmware_filename) {
		if (kvm->cfg.kernel_cmdline)
			_FDT(fdt_property_string(fdt, "bootargs",
						 kvm->cfg.kernel_cmdline));
	} else
		_FDT(fdt_property_string(fdt, "bootargs",
					 kvm->cfg.real_cmdline));

	_FDT(fdt_property_u64(fdt, "kaslr-seed", kvm->cfg.arch.kaslr_seed));
	_FDT(fdt_property_string(fdt, "stdout-path", "serial0"));

	/* Initrd */
	if (kvm->arch.initrd_size != 0) {
		u64 ird_st_prop = cpu_to_fdt64(kvm->arch.initrd_guest_start);
		u64 ird_end_prop = cpu_to_fdt64(kvm->arch.initrd_guest_start +
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
	generate_cpu_nodes(fdt, kvm);
	if (generate_cpu_peripheral_fdt_nodes)
		generate_cpu_peripheral_fdt_nodes(fdt, kvm);

	/* Virtio MMIO devices */
	dev_hdr = device__first_dev(DEVICE_BUS_MMIO);
	while (dev_hdr) {
		generate_mmio_fdt_nodes = dev_hdr->data;
		generate_mmio_fdt_nodes(fdt, dev_hdr, generate_irq_prop);
		dev_hdr = device__next_dev(dev_hdr);
	}

	/* IOPORT devices (!) */
	dev_hdr = device__first_dev(DEVICE_BUS_IOPORT);
	while (dev_hdr) {
		generate_mmio_fdt_nodes = dev_hdr->data;
		generate_mmio_fdt_nodes(fdt, dev_hdr, generate_irq_prop);
		dev_hdr = device__next_dev(dev_hdr);
	}

	/* PCI host controller */
	pci__generate_fdt_nodes(fdt);

	/* PSCI firmware */
	_FDT(fdt_begin_node(fdt, "psci"));
	if (kvm__supports_extension(kvm, KVM_CAP_ARM_PSCI_0_2)) {
		const char compatible[] = "arm,psci-0.2\0arm,psci";
		_FDT(fdt_property(fdt, "compatible",
				  compatible, sizeof(compatible)));
		if (kvm->cfg.arch.aarch32_guest)
			fns = &psci_0_2_aarch32_fns;
		else
			fns = &psci_0_2_aarch64_fns;
	} else {
		_FDT(fdt_property_string(fdt, "compatible", "arm,psci"));
		fns = &psci_0_1_fns;
	}
	_FDT(fdt_property_string(fdt, "method", "hvc"));
	_FDT(fdt_property_cell(fdt, "cpu_suspend", fns->cpu_suspend));
	_FDT(fdt_property_cell(fdt, "cpu_off", fns->cpu_off));
	_FDT(fdt_property_cell(fdt, "cpu_on", fns->cpu_on));
	_FDT(fdt_property_cell(fdt, "migrate", fns->migrate));
	_FDT(fdt_end_node(fdt));

	if (fdt_stdout_path) {
		_FDT(fdt_begin_node(fdt, "aliases"));
		_FDT(fdt_property_string(fdt, "serial0", fdt_stdout_path));
		_FDT(fdt_end_node(fdt));

		free(fdt_stdout_path);
		fdt_stdout_path = NULL;
	}

	/* Finalise. */
	_FDT(fdt_end_node(fdt));
	_FDT(fdt_finish(fdt));

	_FDT(fdt_open_into(fdt, fdt_dest, FDT_MAX_SIZE));
	_FDT(fdt_pack(fdt_dest));

	if (kvm->cfg.arch.dump_dtb_filename)
		dump_fdt(kvm->cfg.arch.dump_dtb_filename, fdt_dest);
	return 0;
}
late_init(setup_fdt);
