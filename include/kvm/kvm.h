#ifndef KVM__KVM_H
#define KVM__KVM_H

#include "kvm/kvm-arch.h"
#include "kvm/kvm-config.h"
#include "kvm/util-init.h"
#include "kvm/kvm.h"

#include <stdbool.h>
#include <linux/types.h>
#include <time.h>
#include <signal.h>

#define SIGKVMEXIT		(SIGRTMIN + 0)
#define SIGKVMPAUSE		(SIGRTMIN + 1)

#define KVM_PID_FILE_PATH	"/.lkvm/"
#define HOME_DIR		getenv("HOME")
#define KVM_BINARY_NAME		"lkvm"

#define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))

#define DEFINE_KVM_EXT(ext)		\
	.name = #ext,			\
	.code = ext

enum {
	KVM_VMSTATE_RUNNING,
	KVM_VMSTATE_PAUSED,
};

struct kvm_ext {
	const char *name;
	int code;
};

struct kvm {
	struct kvm_arch		arch;
	struct kvm_config	cfg;
	int			sys_fd;		/* For system ioctls(), i.e. /dev/kvm */
	int			vm_fd;		/* For VM ioctls() */
	timer_t			timerid;	/* Posix timer for interrupts */

	int			nrcpus;		/* Number of cpus to run */
	struct kvm_cpu		**cpus;

	u32			mem_slots;	/* for KVM_SET_USER_MEMORY_REGION */
	u64			ram_size;
	void			*ram_start;
	u64			ram_pagesize;

	bool			nmi_disabled;

	const char		*vmlinux;
	struct disk_image       **disks;
	int                     nr_disks;

	int			vm_state;
};

void kvm__set_dir(const char *fmt, ...);
const char *kvm__get_dir(void);

int kvm__init(struct kvm *kvm);
struct kvm *kvm__new(void);
int kvm__recommended_cpus(struct kvm *kvm);
int kvm__max_cpus(struct kvm *kvm);
void kvm__init_ram(struct kvm *kvm);
int kvm__exit(struct kvm *kvm);
bool kvm__load_firmware(struct kvm *kvm, const char *firmware_filename);
bool kvm__load_kernel(struct kvm *kvm, const char *kernel_filename,
			const char *initrd_filename, const char *kernel_cmdline, u16 vidmode);
int kvm_timer__init(struct kvm *kvm);
int kvm_timer__exit(struct kvm *kvm);
void kvm__irq_line(struct kvm *kvm, int irq, int level);
void kvm__irq_trigger(struct kvm *kvm, int irq);
bool kvm__emulate_io(struct kvm *kvm, u16 port, void *data, int direction, int size, u32 count);
bool kvm__emulate_mmio(struct kvm *kvm, u64 phys_addr, u8 *data, u32 len, u8 is_write);
int kvm__register_mem(struct kvm *kvm, u64 guest_phys, u64 size, void *userspace_addr);
int kvm__register_mmio(struct kvm *kvm, u64 phys_addr, u64 phys_addr_len, bool coalesce,
			void (*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr),
			void *ptr);
bool kvm__deregister_mmio(struct kvm *kvm, u64 phys_addr);
void kvm__pause(struct kvm *kvm);
void kvm__continue(struct kvm *kvm);
void kvm__notify_paused(void);
int kvm__get_sock_by_instance(const char *name);
int kvm__enumerate_instances(int (*callback)(const char *name, int pid));
void kvm__remove_socket(const char *name);

void kvm__arch_set_cmdline(char *cmdline, bool video);
void kvm__arch_init(struct kvm *kvm, const char *hugetlbfs_path, u64 ram_size);
void kvm__arch_delete_ram(struct kvm *kvm);
int kvm__arch_setup_firmware(struct kvm *kvm);
int kvm__arch_free_firmware(struct kvm *kvm);
bool kvm__arch_cpu_supports_vm(void);
void kvm__arch_periodic_poll(struct kvm *kvm);

int load_flat_binary(struct kvm *kvm, int fd_kernel, int fd_initrd, const char *kernel_cmdline);
bool load_bzimage(struct kvm *kvm, int fd_kernel, int fd_initrd, const char *kernel_cmdline, u16 vidmode);

/*
 * Debugging
 */
void kvm__dump_mem(struct kvm *kvm, unsigned long addr, unsigned long size);

extern const char *kvm_exit_reasons[];

static inline bool host_ptr_in_ram(struct kvm *kvm, void *p)
{
	return kvm->ram_start <= p && p < (kvm->ram_start + kvm->ram_size);
}

static inline void *guest_flat_to_host(struct kvm *kvm, unsigned long offset)
{
	return kvm->ram_start + offset;
}

bool kvm__supports_extension(struct kvm *kvm, unsigned int extension);

#endif /* KVM__KVM_H */
