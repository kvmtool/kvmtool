#ifndef ARM_COMMON__GIC_H
#define ARM_COMMON__GIC_H

#define GIC_SGI_IRQ_BASE		0
#define GIC_PPI_IRQ_BASE		16
#define GIC_SPI_IRQ_BASE		32

#define GIC_FDT_IRQ_NUM_CELLS		3

#define GIC_FDT_IRQ_TYPE_SPI		0
#define GIC_FDT_IRQ_TYPE_PPI		1

#define GIC_FDT_IRQ_FLAGS_EDGE_LO_HI	1
#define GIC_FDT_IRQ_FLAGS_EDGE_HI_LO	2
#define GIC_FDT_IRQ_FLAGS_LEVEL_HI	4
#define GIC_FDT_IRQ_FLAGS_LEVEL_LO	8

#define GIC_FDT_IRQ_PPI_CPU_SHIFT	8
#define GIC_FDT_IRQ_PPI_CPU_MASK	(0xff << GIC_FDT_IRQ_PPI_CPU_SHIFT)

#define GIC_CPUI_CTLR_EN		(1 << 0)
#define GIC_CPUI_PMR_MIN_PRIO		0xff

#define GIC_CPUI_OFF_PMR		4

#define GIC_MAX_CPUS			8
#define GIC_MAX_IRQ			255

#ifndef __ASSEMBLY__
struct kvm;

int gic__alloc_irqnum(void);
int gic__init_irqchip(struct kvm *kvm);
void gic__generate_fdt_nodes(void *fdt, u32 phandle);

#endif /* __ASSEMBLY__ */
#endif /* ARM_COMMON__GIC_H */
