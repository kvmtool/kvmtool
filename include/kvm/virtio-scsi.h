#ifndef KVM__SCSI_VIRTIO_H
#define KVM__SCSI_VIRTIO_H

#include "kvm/disk-image.h"

struct kvm;

int virtio_scsi_init(struct kvm *kvm);
int virtio_scsi_exit(struct kvm *kvm);

#endif /* KVM__SCSI_VIRTIO_H */
