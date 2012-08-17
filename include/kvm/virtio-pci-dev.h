#ifndef VIRTIO_PCI_DEV_H_
#define VIRTIO_PCI_DEV_H_

#include <linux/virtio_ids.h>

/*
 * Virtio PCI device constants and resources
 * they do use (such as irqs and pins).
 */

#define PCI_DEVICE_ID_VIRTIO_NET		0x1000
#define PCI_DEVICE_ID_VIRTIO_BLK		0x1001
#define PCI_DEVICE_ID_VIRTIO_CONSOLE		0x1003
#define PCI_DEVICE_ID_VIRTIO_RNG		0x1004
#define PCI_DEVICE_ID_VIRTIO_BLN		0x1005
#define PCI_DEVICE_ID_VIRTIO_SCSI		0x1008
#define PCI_DEVICE_ID_VIRTIO_9P			0x1009
#define PCI_DEVICE_ID_VESA			0x2000
#define PCI_DEVICE_ID_PCI_SHMEM			0x0001

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4
#define PCI_VENDOR_ID_PCI_SHMEM			0x0001
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET	0x1af4

#define PCI_SUBSYSTEM_ID_VESA			0x0004
#define PCI_SUBSYSTEM_ID_PCI_SHMEM		0x0001

#define PCI_CLASS_BLK				0x018000
#define PCI_CLASS_NET				0x020000
#define PCI_CLASS_CONSOLE			0x078000
/*
 * 0xFF Device does not fit in any defined classes
 */
#define PCI_CLASS_RNG				0xff0000
#define PCI_CLASS_BLN				0xff0000
#define PCI_CLASS_9P				0xff0000

#endif /* VIRTIO_PCI_DEV_H_ */
