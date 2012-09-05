#ifndef KVM__VIRTIO_9P_H
#define KVM__VIRTIO_9P_H
#include "kvm/virtio.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/parse-options.h"

#include <sys/types.h>
#include <dirent.h>
#include <linux/list.h>
#include <linux/rbtree.h>

#define NUM_VIRT_QUEUES		1
#define VIRTQUEUE_NUM		128
#define	VIRTIO_9P_DEFAULT_TAG	"kvm_9p"
#define VIRTIO_9P_HDR_LEN	(sizeof(u32)+sizeof(u8)+sizeof(u16))
#define VIRTIO_9P_VERSION_DOTL	"9P2000.L"
#define MAX_TAG_LEN		32

struct p9_msg {
	u32			size;
	u8			cmd;
	u16			tag;
	u8			msg[0];
} __attribute__((packed));

struct p9_fid {
	u32			fid;
	u32			uid;
	char			abs_path[PATH_MAX];
	char			*path;
	DIR			*dir;
	int			fd;
	struct rb_node		node;
};

struct p9_dev_job {
	struct virt_queue	*vq;
	struct p9_dev		*p9dev;
	struct thread_pool__job job_id;
};

struct p9_dev {
	struct list_head	list;
	struct virtio_device	vdev;
	struct rb_root		fids;

	struct virtio_9p_config	*config;
	u32			features;

	/* virtio queue */
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
	struct p9_dev_job	jobs[NUM_VIRT_QUEUES];
	char			root_dir[PATH_MAX];
};

struct p9_pdu {
	u32			queue_head;
	size_t			read_offset;
	size_t			write_offset;
	u16			out_iov_cnt;
	u16			in_iov_cnt;
	struct iovec		in_iov[VIRTQUEUE_NUM];
	struct iovec		out_iov[VIRTQUEUE_NUM];
};

struct kvm;

int virtio_9p_rootdir_parser(const struct option *opt, const char *arg, int unset);
int virtio_9p_img_name_parser(const struct option *opt, const char *arg, int unset);
int virtio_9p__register(struct kvm *kvm, const char *root, const char *tag_name);
int virtio_9p__init(struct kvm *kvm);
int virtio_p9_pdu_readf(struct p9_pdu *pdu, const char *fmt, ...);
int virtio_p9_pdu_writef(struct p9_pdu *pdu, const char *fmt, ...);

#endif
