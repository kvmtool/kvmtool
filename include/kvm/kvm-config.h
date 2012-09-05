#ifndef KVM_CONFIG_H_
#define KVM_CONFIG_H_

#include "kvm/disk-image.h"

#define DEFAULT_KVM_DEV		"/dev/kvm"
#define DEFAULT_CONSOLE		"serial"
#define DEFAULT_NETWORK		"user"
#define DEFAULT_HOST_ADDR	"192.168.33.1"
#define DEFAULT_GUEST_ADDR	"192.168.33.15"
#define DEFAULT_GUEST_MAC	"02:15:15:15:15:15"
#define DEFAULT_HOST_MAC	"02:01:01:01:01:01"
#define DEFAULT_SCRIPT		"none"
#define DEFAULT_SANDBOX_FILENAME "guest/sandbox.sh"

#define MIN_RAM_SIZE_MB		(64ULL)
#define MIN_RAM_SIZE_BYTE	(MIN_RAM_SIZE_MB << MB_SHIFT)

struct kvm_config {
	struct disk_image_params disk_image[MAX_DISK_IMAGES];
	u64 ram_size;
	u8  image_count;
	u8 num_net_devices;
	bool virtio_rng;
	int active_console;
	int debug_iodelay;
	int nrcpus;
	int vidmode;
	const char *kernel_cmdline;
	const char *kernel_filename;
	const char *vmlinux_filename;
	const char *initrd_filename;
	const char *firmware_filename;
	const char *console;
	const char *dev;
	const char *network;
	const char *host_ip;
	const char *guest_ip;
	const char *guest_mac;
	const char *host_mac;
	const char *script;
	const char *guest_name;
	const char *sandbox;
	const char *hugetlbfs_path;
	const char *custom_rootfs_name;
	const char *real_cmdline;
	struct virtio_net_params *net_params;
	bool single_step;
	bool vnc;
	bool sdl;
	bool balloon;
	bool using_rootfs;
	bool custom_rootfs;
	bool no_net;
	bool no_dhcp;
	bool ioport_debug;
	bool mmio_debug;
};

#endif
