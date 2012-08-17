#ifndef KVM__SCSI_VIRTIO_H
#define KVM__SCSI_VIRTIO_H

#include "kvm/disk-image.h"

struct kvm;

int virtio_scsi_init(struct kvm *kvm);
int virtio_scsi_exit(struct kvm *kvm);

/*----------------------------------------------------*/
/* TODO: Remove this when tcm_vhost goes upstream */
#define TRANSPORT_IQN_LEN		224
#define VHOST_SCSI_ABI_VERSION		0
struct vhost_scsi_target {
	int abi_version;
	unsigned char vhost_wwpn[TRANSPORT_IQN_LEN];
	unsigned short vhost_tpgt;
};
/* VHOST_SCSI specific defines */
#define VHOST_SCSI_SET_ENDPOINT _IOW(VHOST_VIRTIO, 0x40, struct vhost_scsi_target)
#define VHOST_SCSI_CLEAR_ENDPOINT _IOW(VHOST_VIRTIO, 0x41, struct vhost_scsi_target)
#define VHOST_SCSI_GET_ABI_VERSION _IOW(VHOST_VIRTIO, 0x42, struct vhost_scsi_target)
/*----------------------------------------------------*/

#endif /* KVM__SCSI_VIRTIO_H */
