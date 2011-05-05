#ifndef KVM__BLK_VIRTIO_H
#define KVM__BLK_VIRTIO_H

#include "kvm/disk-image.h"

struct kvm;

void virtio_blk__init(struct kvm *self, struct disk_image *disk);

#endif /* KVM__BLK_VIRTIO_H */
