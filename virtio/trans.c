#include "kvm/virtio-trans.h"

#include "kvm/virtio-pci.h"
#include "kvm/util.h"

#include <stdlib.h>

int virtio_trans_init(struct virtio_trans *vtrans, enum virtio_trans_type type)
{
	void *trans;

	switch (type) {
	case VIRTIO_PCI:
		trans = calloc(sizeof(struct virtio_pci), 1);
		if (!trans)
			return -ENOMEM;
		vtrans->virtio = trans;
		vtrans->trans_ops = virtio_pci__get_trans_ops();
	default:
		return -1;
	};

	return 0;
}