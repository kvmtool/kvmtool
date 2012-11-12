/*
 * SPAPR PHB emulation, RTAS interface to PCI config space, device tree nodes
 * for enumerated devices.
 *
 * Borrowed heavily from QEMU's spapr_pci.c,
 * Copyright (c) 2011 Alexey Kardashevskiy, IBM Corporation.
 * Copyright (c) 2011 David Gibson, IBM Corporation.
 *
 * Modifications copyright 2011 Matt Evans <matt@ozlabs.org>, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include "spapr.h"
#include "spapr_pci.h"
#include "kvm/fdt.h"
#include "kvm/util.h"
#include "kvm/pci.h"

#include <linux/pci_regs.h>
#include <linux/byteorder.h>


/* #define DEBUG_PHB yes */
#ifdef DEBUG_PHB
#define phb_dprintf(fmt, ...)					\
	do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define phb_dprintf(fmt, ...)			\
	do { } while (0)
#endif

static const uint32_t bars[] = {
	PCI_BASE_ADDRESS_0, PCI_BASE_ADDRESS_1,
	PCI_BASE_ADDRESS_2, PCI_BASE_ADDRESS_3,
	PCI_BASE_ADDRESS_4, PCI_BASE_ADDRESS_5
	/*, PCI_ROM_ADDRESS*/
};

#define PCI_NUM_REGIONS		7

/* Macros to operate with address in OF binding to PCI */
#define b_x(x, p, l)	(((x) & ((1<<(l))-1)) << (p))
#define b_n(x)		b_x((x), 31, 1) /* 0 if relocatable */
#define b_p(x)		b_x((x), 30, 1) /* 1 if prefetchable */
#define b_t(x)		b_x((x), 29, 1) /* 1 if the address is aliased */
#define b_ss(x)		b_x((x), 24, 2) /* the space code */
#define b_bbbbbbbb(x)	b_x((x), 16, 8) /* bus number */
#define b_ddddd(x)	b_x((x), 11, 5) /* device number */
#define b_fff(x)	b_x((x), 8, 3)	/* function number */
#define b_rrrrrrrr(x)	b_x((x), 0, 8)	/* register number */

#define SS_M64		3
#define SS_M32		2
#define SS_IO		1
#define SS_CONFIG	0


static struct spapr_phb phb;


static void rtas_ibm_read_pci_config(struct kvm_cpu *vcpu,
				     uint32_t token, uint32_t nargs,
				     target_ulong args,
				     uint32_t nret, target_ulong rets)
{
	uint32_t val = 0;
	uint64_t buid = ((uint64_t)rtas_ld(vcpu->kvm, args, 1) << 32) | rtas_ld(vcpu->kvm, args, 2);
	union pci_config_address addr = { .w = rtas_ld(vcpu->kvm, args, 0) };
	struct pci_device_header *dev = pci__find_dev(addr.device_number);
	uint32_t size = rtas_ld(vcpu->kvm, args, 3);

	if (buid != phb.buid || !dev || (size > 4)) {
		phb_dprintf("- cfgRd buid 0x%lx cfg addr 0x%x size %d not found\n",
			    buid, addr.w, size);

		rtas_st(vcpu->kvm, rets, 0, -1);
		return;
	}
	pci__config_rd(vcpu->kvm, addr, &val, size);
	/* It appears this wants a byteswapped result... */
	switch (size) {
	case 4:
		val = le32_to_cpu(val);
		break;
	case 2:
		val = le16_to_cpu(val>>16);
		break;
	case 1:
		val = val >> 24;
		break;
	}
	phb_dprintf("- cfgRd buid 0x%lx addr 0x%x (/%d): b%d,d%d,f%d,r0x%x, val 0x%x\n",
		    buid, addr.w, size, addr.bus_number, addr.device_number, addr.function_number,
		    addr.register_number, val);

	rtas_st(vcpu->kvm, rets, 0, 0);
	rtas_st(vcpu->kvm, rets, 1, val);
}

static void rtas_read_pci_config(struct kvm_cpu *vcpu,
				 uint32_t token, uint32_t nargs,
				 target_ulong args,
				 uint32_t nret, target_ulong rets)
{
	uint32_t val;
	union pci_config_address addr = { .w = rtas_ld(vcpu->kvm, args, 0) };
	struct pci_device_header *dev = pci__find_dev(addr.device_number);
	uint32_t size = rtas_ld(vcpu->kvm, args, 1);

	if (!dev || (size > 4)) {
		rtas_st(vcpu->kvm, rets, 0, -1);
		return;
	}
	pci__config_rd(vcpu->kvm, addr, &val, size);
	switch (size) {
	case 4:
		val = le32_to_cpu(val);
		break;
	case 2:
		val = le16_to_cpu(val>>16); /* We're yuck-endian. */
		break;
	case 1:
		val = val >> 24;
		break;
	}
	phb_dprintf("- cfgRd addr 0x%x size %d, val 0x%x\n", addr.w, size, val);
	rtas_st(vcpu->kvm, rets, 0, 0);
	rtas_st(vcpu->kvm, rets, 1, val);
}

static void rtas_ibm_write_pci_config(struct kvm_cpu *vcpu,
				      uint32_t token, uint32_t nargs,
				      target_ulong args,
				      uint32_t nret, target_ulong rets)
{
	uint64_t buid = ((uint64_t)rtas_ld(vcpu->kvm, args, 1) << 32) | rtas_ld(vcpu->kvm, args, 2);
	union pci_config_address addr = { .w = rtas_ld(vcpu->kvm, args, 0) };
	struct pci_device_header *dev = pci__find_dev(addr.device_number);
	uint32_t size = rtas_ld(vcpu->kvm, args, 3);
	uint32_t val = rtas_ld(vcpu->kvm, args, 4);

	if (buid != phb.buid || !dev || (size > 4)) {
		phb_dprintf("- cfgWr buid 0x%lx cfg addr 0x%x/%d error (val 0x%x)\n",
			    buid, addr.w, size, val);

		rtas_st(vcpu->kvm, rets, 0, -1);
		return;
	}
	phb_dprintf("- cfgWr buid 0x%lx addr 0x%x (/%d): b%d,d%d,f%d,r0x%x, val 0x%x\n",
		    buid, addr.w, size, addr.bus_number, addr.device_number, addr.function_number,
		    addr.register_number, val);
	switch (size) {
	case 4:
		val = le32_to_cpu(val);
		break;
	case 2:
		val = le16_to_cpu(val) << 16;
		break;
	case 1:
		val = val >> 24;
		break;
	}
	pci__config_wr(vcpu->kvm, addr, &val, size);
	rtas_st(vcpu->kvm, rets, 0, 0);
}

static void rtas_write_pci_config(struct kvm_cpu *vcpu,
				  uint32_t token, uint32_t nargs,
				  target_ulong args,
				  uint32_t nret, target_ulong rets)
{
	union pci_config_address addr = { .w = rtas_ld(vcpu->kvm, args, 0) };
	struct pci_device_header *dev = pci__find_dev(addr.device_number);
	uint32_t size = rtas_ld(vcpu->kvm, args, 1);
	uint32_t val = rtas_ld(vcpu->kvm, args, 2);

	if (!dev || (size > 4)) {
		rtas_st(vcpu->kvm, rets, 0, -1);
		return;
	}

	phb_dprintf("- cfgWr addr 0x%x (/%d): b%d,d%d,f%d,r0x%x, val 0x%x\n",
		    addr.w, size, addr.bus_number, addr.device_number, addr.function_number,
		    addr.register_number, val);
	switch (size) {
	case 4:
		val = le32_to_cpu(val);
		break;
	case 2:
		val = le16_to_cpu(val) << 16;
		break;
	case 1:
		val = val >> 24;
		break;
	}
	pci__config_wr(vcpu->kvm, addr, &val, size);
	rtas_st(vcpu->kvm, rets, 0, 0);
}

void spapr_create_phb(struct kvm *kvm,
		      const char *busname, uint64_t buid,
		      uint64_t mem_win_addr, uint64_t mem_win_size,
		      uint64_t io_win_addr, uint64_t io_win_size)
{
	/*
	 * Since kvmtool doesn't really have any concept of buses etc.,
	 * there's nothing to register here.  Just register RTAS.
	 */
	spapr_rtas_register("read-pci-config", rtas_read_pci_config);
	spapr_rtas_register("write-pci-config", rtas_write_pci_config);
	spapr_rtas_register("ibm,read-pci-config", rtas_ibm_read_pci_config);
	spapr_rtas_register("ibm,write-pci-config", rtas_ibm_write_pci_config);

	phb.buid = buid;
	phb.mem_addr = mem_win_addr;
	phb.mem_size = mem_win_size;
	phb.io_addr  = io_win_addr;
	phb.io_size  = io_win_size;

	kvm->arch.phb = &phb;
}

static uint32_t bar_to_ss(unsigned long bar)
{
	if ((bar & PCI_BASE_ADDRESS_SPACE) ==
	    PCI_BASE_ADDRESS_SPACE_IO)
		return SS_IO;
	else if (bar & PCI_BASE_ADDRESS_MEM_TYPE_64)
		return SS_M64;
	else
		return SS_M32;
}

static unsigned long bar_to_addr(unsigned long bar)
{
	if ((bar & PCI_BASE_ADDRESS_SPACE) ==
	    PCI_BASE_ADDRESS_SPACE_IO)
		return bar & PCI_BASE_ADDRESS_IO_MASK;
	else
		return bar & PCI_BASE_ADDRESS_MEM_MASK;
}

int spapr_populate_pci_devices(struct kvm *kvm,
			       uint32_t xics_phandle,
			       void *fdt)
{
	int bus_off, node_off = 0, devid, fn, i, n, devices;
	char nodename[256];
	struct {
		uint32_t hi;
		uint64_t addr;
		uint64_t size;
	} __attribute__((packed)) reg[PCI_NUM_REGIONS + 1],
		  assigned_addresses[PCI_NUM_REGIONS];
	uint32_t bus_range[] = { cpu_to_be32(0), cpu_to_be32(0xff) };
	struct {
		uint32_t hi;
		uint64_t child;
		uint64_t parent;
		uint64_t size;
	} __attribute__((packed)) ranges[] = {
		{
			cpu_to_be32(b_ss(1)), cpu_to_be64(0),
			cpu_to_be64(phb.io_addr),
			cpu_to_be64(phb.io_size),
		},
		{
			cpu_to_be32(b_ss(2)), cpu_to_be64(0),
			cpu_to_be64(phb.mem_addr),
			cpu_to_be64(phb.mem_size),
		},
	};
	uint64_t bus_reg[] = { cpu_to_be64(phb.buid), 0 };
	uint32_t interrupt_map_mask[] = {
		cpu_to_be32(b_ddddd(-1)|b_fff(-1)), 0x0, 0x0, 0x0};
	uint32_t interrupt_map[SPAPR_PCI_NUM_LSI][7];

	/* Start populating the FDT */
	sprintf(nodename, "pci@%" PRIx64, phb.buid);
	bus_off = fdt_add_subnode(fdt, 0, nodename);
	if (bus_off < 0) {
		die("error making bus subnode, %s\n", fdt_strerror(bus_off));
		return bus_off;
	}

	/* Write PHB properties */
	_FDT(fdt_setprop_string(fdt, bus_off, "device_type", "pci"));
	_FDT(fdt_setprop_string(fdt, bus_off, "compatible", "IBM,Logical_PHB"));
	_FDT(fdt_setprop_cell(fdt, bus_off, "#address-cells", 0x3));
	_FDT(fdt_setprop_cell(fdt, bus_off, "#size-cells", 0x2));
	_FDT(fdt_setprop_cell(fdt, bus_off, "#interrupt-cells", 0x1));
	_FDT(fdt_setprop(fdt, bus_off, "used-by-rtas", NULL, 0));
	_FDT(fdt_setprop(fdt, bus_off, "bus-range", &bus_range, sizeof(bus_range)));
	_FDT(fdt_setprop(fdt, bus_off, "ranges", &ranges, sizeof(ranges)));
	_FDT(fdt_setprop(fdt, bus_off, "reg", &bus_reg, sizeof(bus_reg)));
	_FDT(fdt_setprop(fdt, bus_off, "interrupt-map-mask",
			 &interrupt_map_mask, sizeof(interrupt_map_mask)));

	/* Populate PCI devices and allocate IRQs */
	devices = 0;

	for (devid = 0; devid < PCI_MAX_DEVICES; devid++) {
		uint32_t *irqmap = interrupt_map[devices];
		struct pci_device_header *hdr = pci__find_dev(devid);

		if (!hdr)
			continue;

		fn = 0; /* kvmtool doesn't yet do multifunction devices */

		sprintf(nodename, "pci@%u,%u", devid, fn);

		/* Allocate interrupt from the map */
		if (devid > SPAPR_PCI_NUM_LSI)	{
			die("Unexpected behaviour in spapr_populate_pci_devices,"
			    "wrong devid %u\n", devid);
		}
		irqmap[0] = cpu_to_be32(b_ddddd(devid)|b_fff(fn));
		irqmap[1] = 0;
		irqmap[2] = 0;
		irqmap[3] = 0;
		irqmap[4] = cpu_to_be32(xics_phandle);
		/*
		 * This is nasty; the PCI devs are set up such that their own
		 * header's irq_line indicates the direct XICS IRQ number to
		 * use.  There REALLY needs to be a hierarchical system in place
		 * to 'raise' an IRQ on the bridge which indexes/looks up which
		 * XICS IRQ to fire.
		 */
		irqmap[5] = cpu_to_be32(hdr->irq_line);
		irqmap[6] = cpu_to_be32(0x8);

		/* Add node to FDT */
		node_off = fdt_add_subnode(fdt, bus_off, nodename);
		if (node_off < 0) {
			die("error making node subnode, %s\n", fdt_strerror(bus_off));
			return node_off;
		}

		_FDT(fdt_setprop_cell(fdt, node_off, "vendor-id",
				      le16_to_cpu(hdr->vendor_id)));
		_FDT(fdt_setprop_cell(fdt, node_off, "device-id",
				      le16_to_cpu(hdr->device_id)));
		_FDT(fdt_setprop_cell(fdt, node_off, "revision-id",
				      hdr->revision_id));
		_FDT(fdt_setprop_cell(fdt, node_off, "class-code",
				      hdr->class[0] | (hdr->class[1] << 8) | (hdr->class[2] << 16)));
		_FDT(fdt_setprop_cell(fdt, node_off, "subsystem-id",
				      le16_to_cpu(hdr->subsys_id)));
		_FDT(fdt_setprop_cell(fdt, node_off, "subsystem-vendor-id",
				      le16_to_cpu(hdr->subsys_vendor_id)));

		/* Config space region comes first */
		reg[0].hi = cpu_to_be32(
			b_n(0) |
			b_p(0) |
			b_t(0) |
			b_ss(SS_CONFIG) |
			b_bbbbbbbb(0) |
			b_ddddd(devid) |
			b_fff(fn));
		reg[0].addr = 0;
		reg[0].size = 0;

		n = 0;
		/* Six BARs, no ROM supported, addresses are 32bit */
		for (i = 0; i < 6; ++i) {
			if (0 == hdr->bar[i]) {
				continue;
			}

			reg[n+1].hi = cpu_to_be32(
				b_n(0) |
				b_p(0) |
				b_t(0) |
				b_ss(bar_to_ss(le32_to_cpu(hdr->bar[i]))) |
				b_bbbbbbbb(0) |
				b_ddddd(devid) |
				b_fff(fn) |
				b_rrrrrrrr(bars[i]));
			reg[n+1].addr = 0;
			reg[n+1].size = cpu_to_be64(hdr->bar_size[i]);

			assigned_addresses[n].hi = cpu_to_be32(
				b_n(1) |
				b_p(0) |
				b_t(0) |
				b_ss(bar_to_ss(le32_to_cpu(hdr->bar[i]))) |
				b_bbbbbbbb(0) |
				b_ddddd(devid) |
				b_fff(fn) |
				b_rrrrrrrr(bars[i]));

			/*
			 * Writing zeroes to assigned_addresses causes the guest kernel to
			 * reassign BARs
			 */
			assigned_addresses[n].addr = cpu_to_be64(bar_to_addr(le32_to_cpu(hdr->bar[i])));
			assigned_addresses[n].size = reg[n+1].size;

			++n;
		}
		_FDT(fdt_setprop(fdt, node_off, "reg", reg, sizeof(reg[0])*(n+1)));
		_FDT(fdt_setprop(fdt, node_off, "assigned-addresses",
				 assigned_addresses,
				 sizeof(assigned_addresses[0])*(n)));
		_FDT(fdt_setprop_cell(fdt, node_off, "interrupts",
				      hdr->irq_pin));

		/* We don't set ibm,dma-window property as we don't have an IOMMU. */

		++devices;
	}

	/* Write interrupt map */
	_FDT(fdt_setprop(fdt, bus_off, "interrupt-map", &interrupt_map,
			 devices * sizeof(interrupt_map[0])));

	return 0;
}
