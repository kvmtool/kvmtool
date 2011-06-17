#include "kvm/virtio-9p.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/virtio.h"
#include "kvm/ioport.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/pci.h"
#include "kvm/threadpool.h"
#include "kvm/irq.h"

#include <linux/virtio_ring.h>
#include <linux/virtio_9p.h>
#include <net/9p/9p.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <dirent.h>

#define NUM_VIRT_QUEUES		1
#define VIRTIO_P9_QUEUE_SIZE	128
#define	VIRTIO_P9_DEFAULT_TAG	"kvm_9p"
#define VIRTIO_P9_HDR_LEN	(sizeof(u32)+sizeof(u8)+sizeof(u16))
#define VIRTIO_P9_MAX_FID	128
#define VIRTIO_P9_VERSION	"9P2000"
#define MAX_TAG_LEN		32


struct p9_msg {
	u32			size;
	u8			cmd;
	u16			tag;
	u8			msg[0];
} __attribute__((packed));

struct p9_fid {
	u32			fid;
	u8			is_dir;
	char			abs_path[PATH_MAX];
	char			*path;
	DIR			*dir;
	int			fd;
};

struct p9_dev_job {
	struct virt_queue		*vq;
	struct p9_dev			*p9dev;
	void				*job_id;
};

struct p9_dev {
	u8			status;
	u8			isr;
	u16			config_vector;
	u32			features;
	struct virtio_9p_config	*config;
	u16			base_addr;

	/* virtio queue */
	u16			queue_selector;
	struct virt_queue	vqs[NUM_VIRT_QUEUES];
	struct p9_dev_job	jobs[NUM_VIRT_QUEUES];
	struct p9_fid		fids[VIRTIO_P9_MAX_FID];
	char			root_dir[PATH_MAX];
	struct pci_device_header pci_hdr;
};

/* Warning: Immediately use value returned from this function */
static const char *rel_to_abs(struct p9_dev *p9dev,
			      const char *path, char *abs_path)
{
	sprintf(abs_path, "%s/%s", p9dev->root_dir, path);

	return abs_path;
}

static bool virtio_p9_dev_in(struct p9_dev *p9dev, void *data,
			     unsigned long offset,
			     int size, u32 count)
{
	u8 *config_space = (u8 *) p9dev->config;

	if (size != 1 || count != 1)
		return false;

	ioport__write8(data, config_space[offset - VIRTIO_MSI_CONFIG_VECTOR]);

	return true;
}

static bool virtio_p9_pci_io_in(struct ioport *ioport, struct kvm *kvm,
				u16 port, void *data, int size, u32 count)
{
	bool ret = true;
	unsigned long offset;
	struct p9_dev *p9dev = ioport->priv;


	offset = port - p9dev->base_addr;

	switch (offset) {
	case VIRTIO_PCI_HOST_FEATURES:
		ioport__write32(data, p9dev->features);
		ret = true;
		break;
	case VIRTIO_PCI_GUEST_FEATURES:
	case VIRTIO_PCI_QUEUE_SEL:
	case VIRTIO_PCI_QUEUE_NOTIFY:
		ret = false;
		break;
	case VIRTIO_PCI_QUEUE_PFN:
		ioport__write32(data, p9dev->vqs[p9dev->queue_selector].pfn);
		break;
	case VIRTIO_PCI_QUEUE_NUM:
		ioport__write16(data, VIRTIO_P9_QUEUE_SIZE);
		break;
	case VIRTIO_PCI_STATUS:
		ioport__write8(data, p9dev->status);
		break;
	case VIRTIO_PCI_ISR:
		ioport__write8(data, p9dev->isr);
		kvm__irq_line(kvm, p9dev->pci_hdr.irq_line, VIRTIO_IRQ_LOW);
		p9dev->isr = VIRTIO_IRQ_LOW;
		break;
	default:
		ret = virtio_p9_dev_in(p9dev, data, offset, size, count);
		break;
	};

	return ret;
}

static int omode2uflags(u8 mode)
{
	int ret = 0;

	/* Basic open modes are same as uflags */
	ret = mode & 3;

	/* Everything else is different */
	if (mode & P9_OTRUNC)
		ret |= O_TRUNC;

	if (mode & P9_OAPPEND)
		ret |= O_APPEND;

	if (mode & P9_OEXCL)
		ret |= O_EXCL;

	return ret;
}

static void st2qid(struct stat *st, struct p9_qid *qid)
{
	*qid = (struct p9_qid) {
		.path		= st->st_ino,
		.version	= st->st_mtime,
	};

	if (S_ISDIR(st->st_mode))
		qid->type	|= P9_QTDIR;
}

static void close_fid(struct p9_dev *p9dev, u32 fid)
{
	if (p9dev->fids[fid].fd > 0) {
		close(p9dev->fids[fid].fd);
		p9dev->fids[fid].fd = -1;
	}
	if (p9dev->fids[fid].dir) {
		closedir(p9dev->fids[fid].dir);
		p9dev->fids[fid].dir = NULL;
	}
}

static void set_p9msg_hdr(struct p9_msg *msg, u32 size, u8 cmd, u16 tag)
{
	*msg = (struct p9_msg) {
		.size	= size,
		.tag	= tag,
		.cmd	= cmd,
	};
}

static bool virtio_p9_version(struct p9_dev *p9dev, struct p9_msg *msg,
			      u32 len, struct iovec *iov,
			      int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg = iov[0].iov_base;
	struct p9_rversion *rversion = (struct p9_rversion *)outmsg->msg;

	rversion->msize		= 4096;
	rversion->version.len	= strlen(VIRTIO_P9_VERSION);
	memcpy(&rversion->version.str, VIRTIO_P9_VERSION, rversion->version.len);

	*outlen = VIRTIO_P9_HDR_LEN +
		rversion->version.len + sizeof(u16) + sizeof(u32);
	set_p9msg_hdr(outmsg, *outlen, P9_RVERSION, msg->tag);

	return true;
}

static bool virtio_p9_clunk(struct p9_dev *p9dev, struct p9_msg *msg,
			    u32 len, struct iovec *iov,
			    int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg = iov[0].iov_base;
	struct p9_tclunk *tclunk = (struct p9_tclunk *)msg->msg;

	close_fid(p9dev, tclunk->fid);

	*outlen = VIRTIO_P9_HDR_LEN;
	set_p9msg_hdr(outmsg, *outlen, P9_RCLUNK, msg->tag);

	return true;
}

static bool virtio_p9_open(struct p9_dev *p9dev, struct p9_msg *msg,
			   u32 len, struct iovec *iov,
			   int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg	= iov[0].iov_base;
	struct p9_topen *topen	= (struct p9_topen *)msg->msg;
	struct p9_ropen *ropen	= (struct p9_ropen *)outmsg->msg;
	struct p9_fid *new_fid	= &p9dev->fids[topen->fid];
	struct stat st;

	if (stat(new_fid->abs_path, &st) < 0)
		return false;

	st2qid(&st, &ropen->qid);
	ropen->iounit = 0;

	if (new_fid->is_dir)
		new_fid->dir	= opendir(new_fid->abs_path);
	else
		new_fid->fd	= open(new_fid->abs_path, omode2uflags(topen->mode));

	*outlen = VIRTIO_P9_HDR_LEN + sizeof(*ropen);
	set_p9msg_hdr(outmsg, *outlen, P9_ROPEN, msg->tag);

	return true;
}

static bool virtio_p9_create(struct p9_dev *p9dev, struct p9_msg *msg,
			     u32 len, struct iovec *iov,
			     int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg		= iov[0].iov_base;
	struct p9_tcreate *tcreate	= (struct p9_tcreate *)msg->msg;
	struct p9_rcreate *rcreate	= (struct p9_rcreate *)outmsg->msg;
	struct p9_fid *fid		= &p9dev->fids[tcreate->fid];
	struct stat st;
	u8 mode;
	u32 perm;

	rcreate->iounit = 0;

	/* Get last byte of the variable length struct */
	mode = *((u8 *)msg + msg->size - 1);
	perm = *(u32 *)((u8 *)msg + msg->size - 5);

	sprintf(fid->path, "%s/%.*s", fid->path, tcreate->name.len, (char *)&tcreate->name.str);

	close_fid(p9dev, tcreate->fid);

	if (perm & P9_DMDIR) {
		mkdir(fid->abs_path, perm & 0xFFFF);
		fid->dir = opendir(fid->abs_path);
		fid->is_dir = 1;
	} else {
		fid->fd = open(fid->abs_path, omode2uflags(mode) | O_CREAT, 0777);
	}

	if (stat(fid->abs_path, &st) < 0)
		return false;

	st2qid(&st, &rcreate->qid);

	*outlen = VIRTIO_P9_HDR_LEN + sizeof(*rcreate);
	set_p9msg_hdr(outmsg, *outlen, P9_RCREATE, msg->tag);

	return true;
}

static bool virtio_p9_walk(struct p9_dev *p9dev, struct p9_msg *msg,
			   u32 len, struct iovec *iov,
			   int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg	= iov[0].iov_base;
	struct p9_twalk *twalk	= (struct p9_twalk *)msg->msg;
	struct p9_rwalk *rwalk	= (struct p9_rwalk *)outmsg->msg;
	struct p9_str *str	= twalk->wnames;
	struct p9_fid *new_fid	= &p9dev->fids[twalk->newfid];
	u8 i;

	rwalk->nwqid = 0;
	if (twalk->nwname) {
		struct p9_fid *fid = &p9dev->fids[twalk->fid];

		for (i = 0; i < twalk->nwname; i++) {
			char tmp[PATH_MAX] = {0};
			char full_path[PATH_MAX];
			struct stat st;

			/* Format the new path we're 'walk'ing into */
			sprintf(tmp, "%s/%.*s", fid->path, str->len, (char *)&str->str);

			if (stat(rel_to_abs(p9dev, tmp, full_path), &st) < 0)
				break;

			st2qid(&st, &rwalk->wqids[i]);
			new_fid->is_dir = S_ISDIR(st.st_mode);
			strcpy(new_fid->path, tmp);
			new_fid->fid = twalk->newfid;
			rwalk->nwqid++;
		}
	} else {
		new_fid->is_dir = p9dev->fids[twalk->fid].is_dir;
		strcpy(new_fid->path, p9dev->fids[twalk->fid].path);
		new_fid->fid	= twalk->newfid;
	}

	*outlen = VIRTIO_P9_HDR_LEN + sizeof(u16) + sizeof(struct p9_qid) * rwalk->nwqid;
	set_p9msg_hdr(outmsg, *outlen, P9_RWALK, msg->tag);

	return true;
}

static bool virtio_p9_attach(struct p9_dev *p9dev, struct p9_msg *msg,
			     u32 len, struct iovec *iov,
			     int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg = iov[0].iov_base;
	struct p9_rattach *rattach = (struct p9_rattach *)outmsg->msg;
	struct p9_tattach *tattach = (struct p9_tattach *)msg->msg;
	struct stat st;
	struct p9_fid *fid;
	u32 i;

	/* Reset everything */
	for (i = 0; i < VIRTIO_P9_MAX_FID; i++)
		p9dev->fids[i].fid = P9_NOFID;

	if (stat(p9dev->root_dir, &st) < 0)
		return false;

	st2qid(&st, &rattach->qid);

	fid = &p9dev->fids[tattach->fid];
	fid->fid = tattach->fid;
	fid->is_dir = 1;
	strcpy(fid->path, "/");

	*outlen = VIRTIO_P9_HDR_LEN + sizeof(*rattach);
	set_p9msg_hdr(outmsg, *outlen, P9_RATTACH, msg->tag);

	return true;
}

static u32 virtio_p9_fill_stat(struct p9_dev *p9dev, const char *name,
			       struct stat *st, struct p9_rstat *rstat)
{
	struct p9_str *str;

	rstat->stat.type = 0;
	rstat->stat.dev = 0;
	st2qid(st, &rstat->stat.qid);
	rstat->stat.mode = st->st_mode;
	rstat->stat.length = st->st_size;
	if (S_ISDIR(st->st_mode)) {
		rstat->stat.length = 0;
		rstat->stat.mode |= P9_DMDIR;
	}

	rstat->stat.atime = st->st_atime;
	rstat->stat.mtime = st->st_mtime;

	str = (struct p9_str *)&rstat->stat.name;
	str->len = strlen(name);
	memcpy(&str->str, name, str->len);
	str = (void *)str + str->len + sizeof(u16);

	/* TODO: Pass usernames to the client */
	str->len = 0;
	str = (void *)str + sizeof(u16);
	str->len = 0;
	str = (void *)str + sizeof(u16);
	str->len = 0;
	str = (void *)str + sizeof(u16);

	/*
	 * We subtract a u16 here because rstat->size
	 * doesn't include rstat->size itself
	 */
	rstat->stat.size = (void *)str - (void *)&rstat->stat - sizeof(u16);

	return rstat->stat.size + sizeof(u16);
}

static bool virtio_p9_read(struct p9_dev *p9dev, struct p9_msg *msg,
			   u32 len, struct iovec *iov,
			   int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg	= iov[0].iov_base;
	struct p9_tread *tread	= (struct p9_tread *)msg->msg;
	struct p9_rread *rread	= (struct p9_rread *)outmsg->msg;
	struct p9_rstat *rstat	= (struct p9_rstat *)iov[1].iov_base;
	struct p9_fid *fid	= &p9dev->fids[tread->fid];
	struct stat st;

	rread->count = 0;

	if (fid->is_dir) {
		/* If reading a dir, fill the buffer with p9_stat entries */
		struct dirent *cur = readdir(fid->dir);
		char full_path[PATH_MAX];

		while (cur) {
			u32 read;

			stat(rel_to_abs(p9dev, cur->d_name, full_path), &st);
			read = virtio_p9_fill_stat(p9dev, cur->d_name,
						   &st, rstat);
			rread->count += read;
			rstat = (void *)rstat + read;
			cur = readdir(fid->dir);
		}
	} else {
		iov[0].iov_base += VIRTIO_P9_HDR_LEN + sizeof(u32);
		iov[0].iov_len -= VIRTIO_P9_HDR_LEN + sizeof(u32);
		rread->count = preadv(fid->fd, iov, iniovcnt, tread->offset);
		if (rread->count > tread->count)
			rread->count = tread->count;
	}

	*outlen = VIRTIO_P9_HDR_LEN + sizeof(u32) + rread->count;
	set_p9msg_hdr(outmsg, *outlen, P9_RREAD, msg->tag);

	return true;
}

static bool virtio_p9_stat(struct p9_dev *p9dev, struct p9_msg *msg,
			   u32 len, struct iovec *iov,
			   int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg = iov[0].iov_base;
	struct p9_tstat *tstat = (struct p9_tstat *)msg->msg;
	struct p9_rstat *rstat = (struct p9_rstat *)(outmsg->msg + sizeof(u16));
	struct stat st;
	struct p9_fid *fid = &p9dev->fids[tstat->fid];
	u32 ret;

	if (stat(fid->abs_path, &st) < 0)
		return false;

	ret = virtio_p9_fill_stat(p9dev, fid->path, &st, rstat);

	*outlen = VIRTIO_P9_HDR_LEN + ret + sizeof(u16) * 2;
	set_p9msg_hdr(outmsg, *outlen, P9_RSTAT, msg->tag);
	return true;
}

static bool virtio_p9_wstat(struct p9_dev *p9dev, struct p9_msg *msg,
			    u32 len, struct iovec *iov,
			    int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg = iov[0].iov_base;
	struct p9_twstat *twstat = (struct p9_twstat *)msg->msg;
	struct p9_str *str;
	struct p9_fid *fid = &p9dev->fids[twstat->fid];
	int res = 0;

	if (twstat->stat.length != -1UL)
		res = ftruncate(fid->fd, twstat->stat.length);

	if (twstat->stat.mode != -1U)
		chmod(fid->abs_path, twstat->stat.mode & 0xFFFF);

	str = (void *)&twstat->stat.name + sizeof(u16);
	if (str->len > 0) {
		char new_name[PATH_MAX] = {0};
		char full_path[PATH_MAX];
		char *last_dir = strrchr(fid->path, '/');

		/* We need to get the full file name out of twstat->name */
		if (last_dir)
			strncpy(new_name, fid->path, last_dir - fid->path + 1);

		memcpy(new_name + strlen(new_name), &str->str, str->len);

		/* fid is reused for the new file */
		rename(fid->abs_path, rel_to_abs(p9dev, new_name, full_path));
		sprintf(fid->path, "%s", new_name);
	}

	*outlen = VIRTIO_P9_HDR_LEN;
	set_p9msg_hdr(outmsg, *outlen, P9_RWSTAT, msg->tag);

	return res == 0;
}

static bool virtio_p9_remove(struct p9_dev *p9dev, struct p9_msg *msg,
			     u32 len, struct iovec *iov,
			     int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg = iov[0].iov_base;
	struct p9_tremove *tremove = (struct p9_tremove *)msg->msg;
	struct p9_fid *fid = &p9dev->fids[tremove->fid];

	close_fid(p9dev, tremove->fid);
	if (fid->is_dir)
		rmdir(fid->abs_path);
	else
		unlink(fid->abs_path);

	*outlen = VIRTIO_P9_HDR_LEN;
	set_p9msg_hdr(outmsg, *outlen, P9_RREMOVE, msg->tag);
	return true;
}

static bool virtio_p9_write(struct p9_dev *p9dev, struct p9_msg *msg,
			    u32 len, struct iovec *iov,
			    int outiovcnt, int iniovcnt, u32 *outlen)
{
	struct p9_msg *outmsg;
	struct p9_rwrite *rwrite;
	struct p9_twrite *twrite = (struct p9_twrite *)msg->msg;
	struct p9_fid *fid = &p9dev->fids[twrite->fid];

	if (outiovcnt == 1) {
		outmsg = iov[0].iov_base;
		rwrite = (struct p9_rwrite *)outmsg->msg;
		rwrite->count = pwrite(fid->fd, &twrite->data,
				       twrite->count, twrite->offset);
	} else {
		outmsg = iov[2].iov_base;
		rwrite = (struct p9_rwrite *)outmsg->msg;
		rwrite->count = pwrite(fid->fd, iov[1].iov_base,
				       twrite->count, twrite->offset);
	}
	*outlen = VIRTIO_P9_HDR_LEN + sizeof(u32);
	set_p9msg_hdr(outmsg, *outlen, P9_RWRITE, msg->tag);

	return true;
}

typedef bool p9_handler(struct p9_dev *p9dev, struct p9_msg *msg,
			u32 len, struct iovec *iov,
			int outiovcnt, int iniovcnt, u32 *outlen);

static p9_handler *virtio_9p_handler [] = {
	[P9_TVERSION] = virtio_p9_version,
	[P9_TATTACH]  = virtio_p9_attach,
	[P9_TSTAT]    = virtio_p9_stat,
	[P9_TCLUNK]   =	virtio_p9_clunk,
	[P9_TWALK]    =	virtio_p9_walk,
	[P9_TOPEN]    =	virtio_p9_open,
	[P9_TREAD]    = virtio_p9_read,
	[P9_TCREATE]  =	virtio_p9_create,
	[P9_TWSTAT]   =	virtio_p9_wstat,
	[P9_TREMOVE]  =	virtio_p9_remove,
	[P9_TWRITE]   =	virtio_p9_write,
};

static bool virtio_p9_do_io_request(struct kvm *kvm, struct p9_dev_job *job)
{
	u32 len = 0;
	u16 out, in, head;
	struct p9_msg *msg;
	p9_handler *handler;
	struct virt_queue *vq;
	struct p9_dev *p9dev;
	struct iovec iov[VIRTIO_P9_QUEUE_SIZE];

	vq = job->vq;
	p9dev = job->p9dev;
	head  = virt_queue__get_iov(vq, iov, &out, &in, kvm);
	msg   = iov[0].iov_base;

	if (msg->cmd >= ARRAY_SIZE(virtio_9p_handler) ||
	    !virtio_9p_handler[msg->cmd]) {
		printf("Unsupported P9 message type: %u\n", msg->cmd);

	} else {
		handler = virtio_9p_handler[msg->cmd];
		handler(p9dev, msg, iov[0].iov_len, iov+1, out, in, &len);
	}
	virt_queue__set_used_elem(vq, head, len);
	return true;
}

static void virtio_p9_do_io(struct kvm *kvm, void *param)
{
	struct p9_dev_job *job = (struct p9_dev_job *)param;
	struct p9_dev *p9dev   = job->p9dev;
	struct virt_queue *vq  = job->vq;

	while (virt_queue__available(vq)) {
		virtio_p9_do_io_request(kvm, job);
		virt_queue__trigger_irq(vq, p9dev->pci_hdr.irq_line,
					&p9dev->isr, kvm);
	}
}

static bool virtio_p9_pci_io_out(struct ioport *ioport, struct kvm *kvm,
				 u16 port, void *data, int size, u32 count)
{
	unsigned long offset;
	bool ret = true;
	struct p9_dev  *p9dev;

	p9dev = ioport->priv;
	offset = port - p9dev->base_addr;

	switch (offset) {
	case VIRTIO_MSI_QUEUE_VECTOR:
	case VIRTIO_PCI_GUEST_FEATURES:
		break;
	case VIRTIO_PCI_QUEUE_PFN: {
		void *p;
		struct p9_dev_job *job;
		struct virt_queue *queue;

		job			= &p9dev->jobs[p9dev->queue_selector];
		queue			= &p9dev->vqs[p9dev->queue_selector];
		queue->pfn		= ioport__read32(data);
		p			= guest_pfn_to_host(kvm, queue->pfn);

		vring_init(&queue->vring, VIRTIO_P9_QUEUE_SIZE, p,
			   VIRTIO_PCI_VRING_ALIGN);

		*job			= (struct p9_dev_job) {
			.vq			= queue,
			.p9dev			= p9dev,
		};
		job->job_id = thread_pool__add_job(kvm, virtio_p9_do_io, job);
		break;
	}
	case VIRTIO_PCI_QUEUE_SEL:
		p9dev->queue_selector	= ioport__read16(data);
		break;
	case VIRTIO_PCI_QUEUE_NOTIFY: {
		u16 queue_index;
		queue_index		= ioport__read16(data);
		thread_pool__do_job(p9dev->jobs[queue_index].job_id);
		break;
	}
	case VIRTIO_PCI_STATUS:
		p9dev->status		= ioport__read8(data);
		break;
	case VIRTIO_MSI_CONFIG_VECTOR:
		p9dev->config_vector	= VIRTIO_MSI_NO_VECTOR;
		break;
	default:
		ret			= false;
		break;
	};

	return ret;
}

static struct ioport_operations virtio_p9_io_ops = {
	.io_in				= virtio_p9_pci_io_in,
	.io_out				= virtio_p9_pci_io_out,
};

void virtio_9p__init(struct kvm *kvm, const char *root, const char *tag_name)
{
	u8 pin, line, dev;
	u32 i, root_len;
	u16 p9_base_addr;
	struct p9_dev *p9dev;

	p9dev = calloc(1, sizeof(*p9dev));
	if (!p9dev)
		return;
	if (!tag_name)
		tag_name = VIRTIO_P9_DEFAULT_TAG;
	p9dev->config = calloc(1, sizeof(*p9dev->config) + strlen(tag_name) + 1);
	if (p9dev->config == NULL)
		goto free_p9dev;

	strcpy(p9dev->root_dir, root);
	root_len = strlen(root);
	/*
	 * We prefix the full path in all fids, This allows us to get the
	 * absolute path of an fid without playing with strings.
	 */
	for (i = 0; i < VIRTIO_P9_MAX_FID; i++) {
		strcpy(p9dev->fids[i].abs_path, root);
		p9dev->fids[i].path = p9dev->fids[i].abs_path + root_len;
	}
	p9dev->config->tag_len = strlen(tag_name);
	if (p9dev->config->tag_len > MAX_TAG_LEN)
		goto free_p9dev_config;

	memcpy(p9dev->config->tag, tag_name, strlen(tag_name));
	p9dev->features |= 1 << VIRTIO_9P_MOUNT_TAG;

	if (irq__register_device(VIRTIO_ID_9P, &dev, &pin, &line) < 0)
		goto free_p9dev_config;

	p9_base_addr			= ioport__register(IOPORT_EMPTY,
							   &virtio_p9_io_ops,
							   IOPORT_SIZE, p9dev);
	p9dev->base_addr		    = p9_base_addr;
	p9dev->pci_hdr = (struct pci_device_header) {
		.vendor_id		= PCI_VENDOR_ID_REDHAT_QUMRANET,
		.device_id		= PCI_DEVICE_ID_VIRTIO_P9,
		.header_type		= PCI_HEADER_TYPE_NORMAL,
		.revision_id		= 0,
		.class			= 0x010000,
		.subsys_vendor_id	= PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET,
		.subsys_id		= VIRTIO_ID_9P,
		.irq_pin		= pin,
		.irq_line		= line,
		.bar[0]			= p9_base_addr | PCI_BASE_ADDRESS_SPACE_IO,
	};
	pci__register(&p9dev->pci_hdr, dev);

	return;
free_p9dev_config:
	free(p9dev->config);
free_p9dev:
	free(p9dev);
}
