#ifndef KVM__BLK_VIRTIO_H
#define KVM__BLK_VIRTIO_H

#include "kvm/disk-image.h"

struct kvm;

int virtio_blk__init(struct kvm *kvm);
int virtio_blk__exit(struct kvm *kvm);
void virtio_blk_complete(void *param, long len);

#endif /* KVM__BLK_VIRTIO_H */
