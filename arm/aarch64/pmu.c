#include <dirent.h>
#include <sched.h>

#include "linux/cpumask.h"
#include "linux/err.h"

#include "kvm/fdt.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util.h"

#include "arm-common/gic.h"

#include "asm/pmu.h"

static bool pmu_has_attr(struct kvm_cpu *vcpu, u64 attr)
{
	struct kvm_device_attr pmu_attr = {
		.group	= KVM_ARM_VCPU_PMU_V3_CTRL,
		.attr	= attr,
	};
	int ret = ioctl(vcpu->vcpu_fd, KVM_HAS_DEVICE_ATTR, &pmu_attr);

	return ret == 0;
}

static void set_pmu_attr(struct kvm_cpu *vcpu, void *addr, u64 attr)
{
	struct kvm_device_attr pmu_attr = {
		.group	= KVM_ARM_VCPU_PMU_V3_CTRL,
		.addr	= (u64)addr,
		.attr	= attr,
	};
	int ret;

	if (pmu_has_attr(vcpu, attr)) {
		ret = ioctl(vcpu->vcpu_fd, KVM_SET_DEVICE_ATTR, &pmu_attr);
		if (ret)
			die_perror("PMU KVM_SET_DEVICE_ATTR");
	} else {
		die_perror("PMU KVM_HAS_DEVICE_ATTR");
	}
}

#define SYS_EVENT_SOURCE	"/sys/bus/event_source/devices/"
/*
 * int is 32 bits and INT_MAX translates in decimal to 2 * 10^9.
 * Make room for newline and NUL.
 */
#define PMU_ID_MAXLEN		12

static int find_pmu_cpumask(struct kvm *kvm, cpumask_t *cpumask)
{
	cpumask_t pmu_cpumask, tmp;
	char buf[PMU_ID_MAXLEN];
	struct dirent *dirent;
	char *cpulist, *path;
	int pmu_id = -ENXIO;
	unsigned long val;
	ssize_t fd_sz;
	int fd, ret;
	DIR *dir;

	memset(buf, 0, sizeof(buf));

	cpulist = calloc(1, PAGE_SIZE);
	if (!cpulist)
		die_perror("calloc");

	path = calloc(1, PAGE_SIZE);
	if (!path)
		die_perror("calloc");

	dir = opendir(SYS_EVENT_SOURCE);
	if (!dir) {
		pmu_id = -errno;
		goto out_free;
	}

	/* Make the compiler happy by copying the NUL terminating byte. */
	strncpy(path, SYS_EVENT_SOURCE, strlen(SYS_EVENT_SOURCE) + 1);

	while ((dirent = readdir(dir))) {
		if (dirent->d_type != DT_LNK)
			continue;

		strcat(path, dirent->d_name);
		strcat(path, "/cpus");
		fd = open(path, O_RDONLY);
		if (fd < 0)
			goto next_dir;

		fd_sz = read_file(fd, cpulist, PAGE_SIZE);
		if (fd_sz < 0) {
			pmu_id = -errno;
			goto out_free;
		}
		close(fd);

		ret = cpulist_parse(cpulist, &pmu_cpumask);
		if (ret) {
			pmu_id = ret;
			goto out_free;
		}

		if (!cpumask_and(&tmp, cpumask, &pmu_cpumask))
			goto next_dir;

		/*
		 * One CPU cannot more than one PMU, hence the set of CPUs which
		 * share PMU A and the set of CPUs which share PMU B are
		 * disjoint. If the target CPUs and the current PMU have at
		 * least one CPU in common, but the target CPUs is not a subset
		 * of the current PMU, then a PMU which is associated with all
		 * the target CPUs does not exist. Stop searching for a PMU when
		 * this happens.
		 */
		if (!cpumask_subset(cpumask, &pmu_cpumask))
			goto out_free;

		strcpy(&path[strlen(path) - 4], "type");
		fd = open(path, O_RDONLY);
		if (fd < 0)
			goto next_dir;

		fd_sz = read_file(fd, buf, PMU_ID_MAXLEN - 1);
		if (fd_sz < 0) {
			pmu_id = -errno;
			goto out_free;
		}
		close(fd);

		val = strtoul(buf, NULL, 10);
		if (val > INT_MAX) {
			pmu_id = -EOVERFLOW;
			goto out_free;
		}
		pmu_id = (int)val;
		pr_debug("Using PMU %s (id %d)", dirent->d_name, pmu_id);
		break;

next_dir:
		/* Reset path. */
		memset(&path[strlen(SYS_EVENT_SOURCE)], '\0',
		       strlen(path) - strlen(SYS_EVENT_SOURCE));
	}

out_free:
	free(path);
	free(cpulist);
	return pmu_id;
}

/*
 * In the case of homogeneous systems, there only one hardware PMU, and all
 * VCPUs will use the same PMU, regardless of the physical CPUs on which the
 * VCPU threads will be executing.
 *
 * For heterogeneous systems, there are 2 ways for the user to ensure that the
 * VM runs on CPUs that have the same PMU:
 *
 * 1. By pinning the entire VM to the desired CPUs, in which case kvmtool will
 * choose the PMU associated with the CPU on which the main thread is executing
 * (the thread that calls find_pmu()).
 *
 * 2. By setting the affinity mask for the VCPUs with the --vcpu-affinity
 * command line argument. All CPUs in the affinity mask must have the same PMU,
 * otherwise kvmtool will not be able to set a PMU.
 */
static int find_pmu(struct kvm *kvm)
{
	cpumask_t *cpumask;
	int i, this_cpu;

	cpumask = calloc(1, cpumask_size());
	if (!cpumask)
		die_perror("calloc");

	if (!kvm->arch.vcpu_affinity_cpuset) {
		this_cpu = sched_getcpu();
		if (this_cpu < 0)
			return -errno;
		cpumask_set_cpu(this_cpu, cpumask);
	} else {
		for (i = 0; i < CPU_SETSIZE; i ++) {
			if (CPU_ISSET(i, kvm->arch.vcpu_affinity_cpuset))
				cpumask_set_cpu(i, cpumask);
		}
	}

	return find_pmu_cpumask(kvm, cpumask);
}

void pmu__generate_fdt_nodes(void *fdt, struct kvm *kvm)
{
	const char compatible[] = "arm,armv8-pmuv3";
	int irq = KVM_ARM_PMUv3_PPI;
	struct kvm_cpu *vcpu;
	int pmu_id = -ENXIO;
	int i;

	u32 cpu_mask = gic__get_fdt_irq_cpumask(kvm);
	u32 irq_prop[] = {
		cpu_to_fdt32(GIC_FDT_IRQ_TYPE_PPI),
		cpu_to_fdt32(irq - 16),
		cpu_to_fdt32(cpu_mask | IRQ_TYPE_LEVEL_HIGH),
	};

	if (!kvm->cfg.arch.has_pmuv3)
		return;

	if (pmu_has_attr(kvm->cpus[0], KVM_ARM_VCPU_PMU_V3_SET_PMU)) {
		pmu_id = find_pmu(kvm);
		if (pmu_id < 0) {
			pr_debug("Failed to find a PMU (errno: %d), "
				 "PMU events might not work", -pmu_id);
		}
	}

	for (i = 0; i < kvm->nrcpus; i++) {
		vcpu = kvm->cpus[i];
		set_pmu_attr(vcpu, &irq, KVM_ARM_VCPU_PMU_V3_IRQ);
		/*
		 * PMU IDs 0-5 are reserved; a positive value means a PMU was
		 * found.
		 */
		if (pmu_id > 0)
			set_pmu_attr(vcpu, &pmu_id, KVM_ARM_VCPU_PMU_V3_SET_PMU);
		set_pmu_attr(vcpu, NULL, KVM_ARM_VCPU_PMU_V3_INIT);
	}

	_FDT(fdt_begin_node(fdt, "pmu"));
	_FDT(fdt_property(fdt, "compatible", compatible, sizeof(compatible)));
	_FDT(fdt_property(fdt, "interrupts", irq_prop, sizeof(irq_prop)));
	_FDT(fdt_end_node(fdt));
}
