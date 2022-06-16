#ifndef ARM_COMMON__GIC_H
#define ARM_COMMON__GIC_H

#define GIC_SGI_IRQ_BASE		0
#define GIC_PPI_IRQ_BASE		16
#define GIC_SPI_IRQ_BASE		32

#define GIC_FDT_IRQ_NUM_CELLS		3

#define GIC_FDT_IRQ_TYPE_SPI		0
#define GIC_FDT_IRQ_TYPE_PPI		1

#define GIC_FDT_IRQ_PPI_CPU_SHIFT	8
#define GIC_FDT_IRQ_PPI_CPU_MASK	(0xff << GIC_FDT_IRQ_PPI_CPU_SHIFT)

#define GIC_CPUI_CTLR_EN		(1 << 0)
#define GIC_CPUI_PMR_MIN_PRIO		0xff

#define GIC_CPUI_OFF_PMR		4

#define GIC_MAX_CPUS			8
#define GIC_MAX_IRQ			255

#define KVM_VGIC_V2M_SIZE		0x1000

enum irqchip_type {
	IRQCHIP_AUTO,
	IRQCHIP_GICV2,
	IRQCHIP_GICV2M,
	IRQCHIP_GICV3,
	IRQCHIP_GICV3_ITS,
};

struct kvm;

int gic__alloc_irqnum(void);
int gic__create(struct kvm *kvm, enum irqchip_type type);
int gic__create_gicv2m_frame(struct kvm *kvm, u64 msi_frame_addr);
void gic__generate_fdt_nodes(void *fdt, enum irqchip_type type);
u32 gic__get_fdt_irq_cpumask(struct kvm *kvm);

int gic__add_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd,
		   int resample_fd);
void gic__del_irqfd(struct kvm *kvm, unsigned int gsi, int trigger_fd);
#define irq__add_irqfd gic__add_irqfd
#define irq__del_irqfd gic__del_irqfd

#endif /* ARM_COMMON__GIC_H */
