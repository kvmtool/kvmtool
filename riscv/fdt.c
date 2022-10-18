#include "kvm/devices.h"
#include "kvm/fdt.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"

#include <stdbool.h>

#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/sizes.h>

struct isa_ext_info {
	const char *name;
	unsigned long ext_id;
};

struct isa_ext_info isa_info_arr[] = {
	{"svpbmt", KVM_RISCV_ISA_EXT_SVPBMT},
	{"sstc", KVM_RISCV_ISA_EXT_SSTC},
	{"svinval", KVM_RISCV_ISA_EXT_SVINVAL},
	{"zihintpause", KVM_RISCV_ISA_EXT_ZIHINTPAUSE},
	{"zicbom", KVM_RISCV_ISA_EXT_ZICBOM},
};

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
#define CPU_ISA_MAX_LEN 128
static void generate_cpu_nodes(void *fdt, struct kvm *kvm)
{
	int cpu, pos, i, index, valid_isa_len;
	const char *valid_isa_order = "IEMAFDQCLBJTPVNSUHKORWXYZG";
	int arr_sz = ARRAY_SIZE(isa_info_arr);
	unsigned long cbom_blksz = 0;

	_FDT(fdt_begin_node(fdt, "cpus"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x1));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x0));
	_FDT(fdt_property_cell(fdt, "timebase-frequency",
				kvm->cpus[0]->riscv_timebase));

	for (cpu = 0; cpu < kvm->nrcpus; ++cpu) {
		char cpu_name[CPU_NAME_MAX_LEN];
		char cpu_isa[CPU_ISA_MAX_LEN];
		struct kvm_cpu *vcpu = kvm->cpus[cpu];
		struct kvm_one_reg reg;
		unsigned long isa_ext_out = 0;

		snprintf(cpu_name, CPU_NAME_MAX_LEN, "cpu@%x", cpu);

		snprintf(cpu_isa, CPU_ISA_MAX_LEN, "rv%ld", vcpu->riscv_xlen);
		pos = strlen(cpu_isa);
		valid_isa_len = strlen(valid_isa_order);
		for (i = 0; i < valid_isa_len; i++) {
			index = valid_isa_order[i] - 'A';
			if (vcpu->riscv_isa & (1 << (index)))
				cpu_isa[pos++] = 'a' + index;
		}

		for (i = 0; i < arr_sz; i++) {
			reg.id = RISCV_ISA_EXT_REG(isa_info_arr[i].ext_id);
			reg.addr = (unsigned long)&isa_ext_out;
			if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
				continue;
			if (!isa_ext_out)
				/* This extension is not available in hardware */
				continue;

			if (kvm->cfg.arch.ext_disabled[isa_info_arr[i].ext_id]) {
				isa_ext_out = 0;
				if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
					pr_warning("Failed to disable %s ISA exension\n",
						   isa_info_arr[i].name);
				continue;
			}

			if (isa_info_arr[i].ext_id == KVM_RISCV_ISA_EXT_ZICBOM && !cbom_blksz) {
				reg.id = RISCV_CONFIG_REG(zicbom_block_size);
				reg.addr = (unsigned long)&cbom_blksz;
				if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
					die("KVM_GET_ONE_REG failed (config.zicbom_block_size)");
			}

			if ((strlen(isa_info_arr[i].name) + pos + 1) >= CPU_ISA_MAX_LEN) {
				pr_warning("Insufficient space to append ISA exension\n");
				break;
			}
			pos += snprintf(cpu_isa + pos, CPU_ISA_MAX_LEN, "_%s",
					isa_info_arr[i].name);
		}
		cpu_isa[pos] = '\0';

		_FDT(fdt_begin_node(fdt, cpu_name));
		_FDT(fdt_property_string(fdt, "device_type", "cpu"));
		_FDT(fdt_property_string(fdt, "compatible", "riscv"));
		if (vcpu->riscv_xlen == 64)
			_FDT(fdt_property_string(fdt, "mmu-type",
						 "riscv,sv48"));
		else
			_FDT(fdt_property_string(fdt, "mmu-type",
						 "riscv,sv32"));
		_FDT(fdt_property_string(fdt, "riscv,isa", cpu_isa));
		if (cbom_blksz)
			_FDT(fdt_property_cell(fdt, "riscv,cbom-block-size", cbom_blksz));
		_FDT(fdt_property_cell(fdt, "reg", cpu));
		_FDT(fdt_property_string(fdt, "status", "okay"));

		_FDT(fdt_begin_node(fdt, "interrupt-controller"));
		_FDT(fdt_property_string(fdt, "compatible", "riscv,cpu-intc"));
		_FDT(fdt_property_cell(fdt, "#interrupt-cells", 1));
		_FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
		_FDT(fdt_property_cell(fdt, "phandle",
					PHANDLE_CPU_INTC_BASE + cpu));
		_FDT(fdt_end_node(fdt));

		_FDT(fdt_end_node(fdt));
	}

	_FDT(fdt_end_node(fdt));
}

static int setup_fdt(struct kvm *kvm)
{
	struct device_header *dev_hdr;
	u8 staging_fdt[FDT_MAX_SIZE];
	u64 mem_reg_prop[]	= {
		cpu_to_fdt64(kvm->arch.memory_guest_start),
		cpu_to_fdt64(kvm->ram_size),
	};
	char *str;
	void *fdt		= staging_fdt;
	void *fdt_dest		= guest_flat_to_host(kvm,
						     kvm->arch.dtb_guest_start);
	void (*generate_mmio_fdt_nodes)(void *, struct device_header *,
					void (*)(void *, u8, enum irq_type));

	/* Create new tree without a reserve map */
	_FDT(fdt_create(fdt, FDT_MAX_SIZE));
	_FDT(fdt_finish_reservemap(fdt));

	/* Header */
	_FDT(fdt_begin_node(fdt, ""));
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

	/* CPUs */
	generate_cpu_nodes(fdt, kvm);

	/* Simple Bus */
	_FDT(fdt_begin_node(fdt, "smb"));
	_FDT(fdt_property_string(fdt, "compatible", "simple-bus"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "interrupt-parent", PHANDLE_PLIC));
	_FDT(fdt_property(fdt, "ranges", NULL, 0));

	/* Virtio MMIO devices */
	dev_hdr = device__first_dev(DEVICE_BUS_MMIO);
	while (dev_hdr) {
		generate_mmio_fdt_nodes = dev_hdr->data;
		generate_mmio_fdt_nodes(fdt, dev_hdr, plic__generate_irq_prop);
		dev_hdr = device__next_dev(dev_hdr);
	}

	/* IOPORT devices */
	dev_hdr = device__first_dev(DEVICE_BUS_IOPORT);
	while (dev_hdr) {
		generate_mmio_fdt_nodes = dev_hdr->data;
		generate_mmio_fdt_nodes(fdt, dev_hdr, plic__generate_irq_prop);
		dev_hdr = device__next_dev(dev_hdr);
	}

	/* PCI host controller */
	pci__generate_fdt_nodes(fdt);

	_FDT(fdt_end_node(fdt));

	if (fdt_stdout_path) {
		str = malloc(strlen(fdt_stdout_path) + strlen("/smb") + 1);
		sprintf(str, "/smb%s", fdt_stdout_path);
		free(fdt_stdout_path);
		fdt_stdout_path = NULL;

		_FDT(fdt_begin_node(fdt, "aliases"));
		_FDT(fdt_property_string(fdt, "serial0", str));
		_FDT(fdt_end_node(fdt));
		free(str);
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
