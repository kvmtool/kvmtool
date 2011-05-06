#ifndef VIRTIO_PCI_DEV_H_
#define VIRTIO_PCI_DEV_H_

/*
 * Virtio PCI device constants and resources
 * they do use (such as irqs and pins).
 */

#define PCI_DEVICE_ID_VIRTIO_NET		0x1000
#define PCI_DEVICE_ID_VIRTIO_BLK		0x1001
#define PCI_DEVICE_ID_VIRTIO_CONSOLE		0x1003
#define PCI_DEVICE_ID_VIRTIO_RNG		0x1004

#define PCI_SUBSYSTEM_ID_VIRTIO_NET		0x0001
#define PCI_SUBSYSTEM_ID_VIRTIO_BLK		0x0002
#define PCI_SUBSYSTEM_ID_VIRTIO_CONSOLE		0x0003
#define PCI_SUBSYSTEM_ID_VIRTIO_RNG		0x0004

#endif /* VIRTIO_PCI_DEV_H_ */
