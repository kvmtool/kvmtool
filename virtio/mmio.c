#include "kvm/devices.h"
#include "kvm/virtio-mmio.h"
#include "kvm/ioeventfd.h"
#include "kvm/ioport.h"
#include "kvm/virtio.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/irq.h"
#include "kvm/fdt.h"

#include <linux/virtio_mmio.h>
#include <string.h>

static u32 virtio_mmio_io_space_blocks = KVM_VIRTIO_MMIO_AREA;

static u32 virtio_mmio_get_io_space_block(u32 size)
{
	u32 block = virtio_mmio_io_space_blocks;
	virtio_mmio_io_space_blocks += size;

	return block;
}

static void virtio_mmio_ioevent_callback(struct kvm *kvm, void *param)
{
	struct virtio_mmio_ioevent_param *ioeventfd = param;
	struct virtio_mmio *vmmio = ioeventfd->vdev->virtio;

	ioeventfd->vdev->ops->notify_vq(kvm, vmmio->dev, ioeventfd->vq);
}

static int virtio_mmio_init_ioeventfd(struct kvm *kvm,
				      struct virtio_device *vdev, u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct ioevent ioevent;
	int err;

	vmmio->ioeventfds[vq] = (struct virtio_mmio_ioevent_param) {
		.vdev		= vdev,
		.vq		= vq,
	};

	ioevent = (struct ioevent) {
		.io_addr	= vmmio->addr + VIRTIO_MMIO_QUEUE_NOTIFY,
		.io_len		= sizeof(u32),
		.fn		= virtio_mmio_ioevent_callback,
		.fn_ptr		= &vmmio->ioeventfds[vq],
		.datamatch	= vq,
		.fn_kvm		= kvm,
		.fd		= eventfd(0, 0),
	};

	if (vdev->use_vhost)
		/*
		 * Vhost will poll the eventfd in host kernel side,
		 * no need to poll in userspace.
		 */
		err = ioeventfd__add_event(&ioevent, 0);
	else
		/* Need to poll in userspace. */
		err = ioeventfd__add_event(&ioevent, IOEVENTFD_FLAG_USER_POLL);
	if (err)
		return err;

	if (vdev->ops->notify_vq_eventfd)
		vdev->ops->notify_vq_eventfd(kvm, vmmio->dev, vq, ioevent.fd);

	return 0;
}

int virtio_mmio_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_VRING;
	kvm__irq_trigger(vmmio->kvm, vmmio->irq);

	return 0;
}

static void virtio_mmio_exit_vq(struct kvm *kvm, struct virtio_device *vdev,
				int vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	ioeventfd__del_event(vmmio->addr + VIRTIO_MMIO_QUEUE_NOTIFY, vq);
	virtio_exit_vq(kvm, vdev, vmmio->dev, vq);
}

int virtio_mmio_signal_config(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_CONFIG;
	kvm__irq_trigger(vmmio->kvm, vmmio->irq);

	return 0;
}

static void virtio_mmio_device_specific(struct kvm_cpu *vcpu,
					u64 addr, u8 *data, u32 len,
					u8 is_write, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 i;

	for (i = 0; i < len; i++) {
		if (is_write)
			vdev->ops->get_config(vmmio->kvm, vmmio->dev)[addr + i] =
					      *(u8 *)data + i;
		else
			data[i] = vdev->ops->get_config(vmmio->kvm,
							vmmio->dev)[addr + i];
	}
}

static void virtio_mmio_config_in(struct kvm_cpu *vcpu,
				  u64 addr, void *data, u32 len,
				  struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct virt_queue *vq;
	u32 val = 0;

	switch (addr) {
	case VIRTIO_MMIO_MAGIC_VALUE:
	case VIRTIO_MMIO_VERSION:
	case VIRTIO_MMIO_DEVICE_ID:
	case VIRTIO_MMIO_VENDOR_ID:
	case VIRTIO_MMIO_STATUS:
	case VIRTIO_MMIO_INTERRUPT_STATUS:
		ioport__write32(data, *(u32 *)(((void *)&vmmio->hdr) + addr));
		break;
	case VIRTIO_MMIO_HOST_FEATURES:
		if (vmmio->hdr.host_features_sel == 0)
			val = vdev->ops->get_host_features(vmmio->kvm,
							   vmmio->dev);
		ioport__write32(data, val);
		break;
	case VIRTIO_MMIO_QUEUE_PFN:
		vq = vdev->ops->get_vq(vmmio->kvm, vmmio->dev,
				       vmmio->hdr.queue_sel);
		ioport__write32(data, vq->pfn);
		break;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		val = vdev->ops->get_size_vq(vmmio->kvm, vmmio->dev,
					     vmmio->hdr.queue_sel);
		ioport__write32(data, val);
		break;
	default:
		break;
	}
}

static void virtio_mmio_config_out(struct kvm_cpu *vcpu,
				   u64 addr, void *data, u32 len,
				   struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct kvm *kvm = vmmio->kvm;
	u32 val = 0;

	switch (addr) {
	case VIRTIO_MMIO_HOST_FEATURES_SEL:
	case VIRTIO_MMIO_GUEST_FEATURES_SEL:
	case VIRTIO_MMIO_QUEUE_SEL:
		val = ioport__read32(data);
		*(u32 *)(((void *)&vmmio->hdr) + addr) = val;
		break;
	case VIRTIO_MMIO_STATUS:
		vmmio->hdr.status = ioport__read32(data);
		if (!vmmio->hdr.status) /* Sample endianness on reset */
			vdev->endian = kvm_cpu__get_endianness(vcpu);
		virtio_notify_status(kvm, vdev, vmmio->dev, vmmio->hdr.status);
		break;
	case VIRTIO_MMIO_GUEST_FEATURES:
		if (vmmio->hdr.guest_features_sel == 0) {
			val = ioport__read32(data);
			virtio_set_guest_features(vmmio->kvm, vdev,
						  vmmio->dev, val);
		}
		break;
	case VIRTIO_MMIO_GUEST_PAGE_SIZE:
		val = ioport__read32(data);
		vmmio->hdr.guest_page_size = val;
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		val = ioport__read32(data);
		vmmio->hdr.queue_num = val;
		vdev->ops->set_size_vq(vmmio->kvm, vmmio->dev,
				       vmmio->hdr.queue_sel, val);
		break;
	case VIRTIO_MMIO_QUEUE_ALIGN:
		val = ioport__read32(data);
		vmmio->hdr.queue_align = val;
		break;
	case VIRTIO_MMIO_QUEUE_PFN:
		val = ioport__read32(data);
		if (val) {
			virtio_mmio_init_ioeventfd(vmmio->kvm, vdev,
						   vmmio->hdr.queue_sel);
			vdev->ops->init_vq(vmmio->kvm, vmmio->dev,
					   vmmio->hdr.queue_sel,
					   vmmio->hdr.guest_page_size,
					   vmmio->hdr.queue_align,
					   val);
		} else {
			virtio_mmio_exit_vq(kvm, vdev, vmmio->hdr.queue_sel);
		}
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		val = ioport__read32(data);
		vdev->ops->notify_vq(vmmio->kvm, vmmio->dev, val);
		break;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		val = ioport__read32(data);
		vmmio->hdr.interrupt_state &= ~val;
		break;
	default:
		break;
	};
}

static void virtio_mmio_mmio_callback(struct kvm_cpu *vcpu,
				      u64 addr, u8 *data, u32 len,
				      u8 is_write, void *ptr)
{
	struct virtio_device *vdev = ptr;
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 offset = addr - vmmio->addr;

	if (offset >= VIRTIO_MMIO_CONFIG) {
		offset -= VIRTIO_MMIO_CONFIG;
		virtio_mmio_device_specific(vcpu, offset, data, len, is_write, ptr);
		return;
	}

	if (is_write)
		virtio_mmio_config_out(vcpu, offset, data, len, ptr);
	else
		virtio_mmio_config_in(vcpu, offset, data, len, ptr);
}

#ifdef CONFIG_HAS_LIBFDT
#define DEVICE_NAME_MAX_LEN 32
static
void generate_virtio_mmio_fdt_node(void *fdt,
				   struct device_header *dev_hdr,
				   void (*generate_irq_prop)(void *fdt,
							     u8 irq,
							     enum irq_type))
{
	char dev_name[DEVICE_NAME_MAX_LEN];
	struct virtio_mmio *vmmio = container_of(dev_hdr,
						 struct virtio_mmio,
						 dev_hdr);
	u64 addr = vmmio->addr;
	u64 reg_prop[] = {
		cpu_to_fdt64(addr),
		cpu_to_fdt64(VIRTIO_MMIO_IO_SIZE),
	};

	snprintf(dev_name, DEVICE_NAME_MAX_LEN, "virtio@%llx", addr);

	_FDT(fdt_begin_node(fdt, dev_name));
	_FDT(fdt_property_string(fdt, "compatible", "virtio,mmio"));
	_FDT(fdt_property(fdt, "reg", reg_prop, sizeof(reg_prop)));
	_FDT(fdt_property(fdt, "dma-coherent", NULL, 0));
	generate_irq_prop(fdt, vmmio->irq, IRQ_TYPE_EDGE_RISING);
	_FDT(fdt_end_node(fdt));
}
#else
static void generate_virtio_mmio_fdt_node(void *fdt,
					  struct device_header *dev_hdr,
					  void (*generate_irq_prop)(void *fdt,
								    u8 irq))
{
	die("Unable to generate device tree nodes without libfdt\n");
}
#endif

void virtio_mmio_assign_irq(struct device_header *dev_hdr)
{
	struct virtio_mmio *vmmio = container_of(dev_hdr,
						 struct virtio_mmio,
						 dev_hdr);
	vmmio->irq = irq__alloc_line();
}

int virtio_mmio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		     int device_id, int subsys_id, int class)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	int r;

	vmmio->addr	= virtio_mmio_get_io_space_block(VIRTIO_MMIO_IO_SIZE);
	vmmio->kvm	= kvm;
	vmmio->dev	= dev;

	r = kvm__register_mmio(kvm, vmmio->addr, VIRTIO_MMIO_IO_SIZE,
			       false, virtio_mmio_mmio_callback, vdev);
	if (r < 0)
		return r;

	vmmio->hdr = (struct virtio_mmio_hdr) {
		.magic		= {'v', 'i', 'r', 't'},
		.version	= 1,
		.device_id	= subsys_id,
		.vendor_id	= 0x4d564b4c , /* 'LKVM' */
		.queue_num_max	= 256,
	};

	vmmio->dev_hdr = (struct device_header) {
		.bus_type	= DEVICE_BUS_MMIO,
		.data		= generate_virtio_mmio_fdt_node,
	};

	r = device__register(&vmmio->dev_hdr);
	if (r < 0) {
		kvm__deregister_mmio(kvm, vmmio->addr);
		return r;
	}

	/*
	 * Instantiate guest virtio-mmio devices using kernel command line
	 * (or module) parameter, e.g
	 *
	 * virtio_mmio.devices=0x200@0xd2000000:5,0x200@0xd2000200:6
	 */
	pr_debug("virtio-mmio.devices=0x%x@0x%x:%d", VIRTIO_MMIO_IO_SIZE,
		 vmmio->addr, vmmio->irq);

	return 0;
}

int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)
{
	int vq;
	struct virtio_mmio *vmmio = vdev->virtio;

	for (vq = 0; vq < vdev->ops->get_vq_count(kvm, vmmio->dev); vq++)
		virtio_mmio_exit_vq(kvm, vdev, vq);

	return 0;
}

int virtio_mmio_exit(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	virtio_mmio_reset(kvm, vdev);
	kvm__deregister_mmio(kvm, vmmio->addr);

	return 0;
}
