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
#include "kvm/ioeventfd.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <dirent.h>

#include <linux/virtio_ring.h>
#include <linux/virtio_9p.h>
#include <net/9p/9p.h>

#define NUM_VIRT_QUEUES		1
#define VIRTQUEUE_NUM		128
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

struct p9_pdu {
	u32 queue_head;
	int offset;
	u16 out_iov_cnt;
	u16 in_iov_cnt;
	struct iovec in_iov[VIRTQUEUE_NUM];
	struct iovec out_iov[VIRTQUEUE_NUM];
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
		ioport__write16(data, VIRTQUEUE_NUM);
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

static u16 virtio_p9_update_iov_cnt(struct iovec iov[], u32 count, int iov_cnt)
{
	int i;
	u32 total = 0;
	for (i = 0; (i < iov_cnt) && (total < count); i++) {
		if (total + iov[i].iov_len > count) {
			/* we don't need this iov fully */
			iov[i].iov_len -= ((total + iov[i].iov_len) - count);
			i++;
			break;
		}
		total += iov[i].iov_len;
	}
	return i;
}

static void virtio_p9_error_reply(struct p9_dev *p9dev,
				  struct p9_pdu *pdu, int err, u32 *outlen)
{
	char *err_str;
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_rerror *rerror  = (struct p9_rerror *)inmsg->msg;

	err_str = strerror(err);
	rerror->error.len = strlen(err_str);
	memcpy(&rerror->error.str, err_str, rerror->error.len);

	*outlen = VIRTIO_P9_HDR_LEN + rerror->error.len + sizeof(u16);
	set_p9msg_hdr(inmsg, *outlen, P9_RERROR, outmsg->tag);
}

static bool virtio_p9_version(struct p9_dev *p9dev,
			      struct p9_pdu *pdu, u32 *outlen)
{
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_rversion *rversion = (struct p9_rversion *)inmsg->msg;

	rversion->msize		= 4096;
	rversion->version.len	= strlen(VIRTIO_P9_VERSION);
	memcpy(&rversion->version.str, VIRTIO_P9_VERSION, rversion->version.len);

	*outlen = VIRTIO_P9_HDR_LEN +
		rversion->version.len + sizeof(u16) + sizeof(u32);
	set_p9msg_hdr(inmsg, *outlen, P9_RVERSION, outmsg->tag);

	return true;
}

static bool virtio_p9_clunk(struct p9_dev *p9dev,
			    struct p9_pdu *pdu, u32 *outlen)
{
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_tclunk *tclunk = (struct p9_tclunk *)outmsg->msg;

	close_fid(p9dev, tclunk->fid);

	*outlen = VIRTIO_P9_HDR_LEN;
	set_p9msg_hdr(inmsg, *outlen, P9_RCLUNK, outmsg->tag);

	return true;
}

static bool virtio_p9_open(struct p9_dev *p9dev,
			   struct p9_pdu *pdu, u32 *outlen)
{
	struct stat st;
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_topen *topen	= (struct p9_topen *)outmsg->msg;
	struct p9_ropen *ropen	= (struct p9_ropen *)inmsg->msg;
	struct p9_fid *new_fid	= &p9dev->fids[topen->fid];

	if (lstat(new_fid->abs_path, &st) < 0)
		goto err_out;

	st2qid(&st, &ropen->qid);
	ropen->iounit = 0;

	if (new_fid->is_dir) {
		new_fid->dir = opendir(new_fid->abs_path);
		if (!new_fid->dir)
			goto err_out;
	} else {
		new_fid->fd  = open(new_fid->abs_path,
				   omode2uflags(topen->mode) | O_NOFOLLOW);
		if (new_fid->fd < 0)
			goto err_out;
	}
	*outlen = VIRTIO_P9_HDR_LEN + sizeof(*ropen);
	set_p9msg_hdr(inmsg, *outlen, P9_ROPEN, outmsg->tag);
	return true;
err_out:
	virtio_p9_error_reply(p9dev, pdu, errno, outlen);
	return true;
}

static bool virtio_p9_create(struct p9_dev *p9dev,
			     struct p9_pdu *pdu, u32 *outlen)
{
	u8 mode;
	u32 perm;
	struct stat st;
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_tcreate *tcreate	= (struct p9_tcreate *)outmsg->msg;
	struct p9_rcreate *rcreate	= (struct p9_rcreate *)inmsg->msg;
	struct p9_fid *fid		= &p9dev->fids[tcreate->fid];


	rcreate->iounit = 0;

	/* Get last byte of the variable length struct */
	mode = *((u8 *)outmsg + outmsg->size - 1);
	perm = *(u32 *)((u8 *)outmsg + outmsg->size - 5);

	sprintf(fid->path, "%s/%.*s", fid->path, tcreate->name.len, (char *)&tcreate->name.str);

	close_fid(p9dev, tcreate->fid);

	if (perm & P9_DMDIR) {
		mkdir(fid->abs_path, perm & 0xFFFF);
		fid->dir = opendir(fid->abs_path);
		fid->is_dir = 1;
	} else {
		fid->fd = open(fid->abs_path, omode2uflags(mode) | O_CREAT, 0777);
	}

	if (lstat(fid->abs_path, &st) < 0)
		return false;

	st2qid(&st, &rcreate->qid);

	*outlen = VIRTIO_P9_HDR_LEN + sizeof(*rcreate);
	set_p9msg_hdr(inmsg, *outlen, P9_RCREATE, outmsg->tag);

	return true;
}

static bool virtio_p9_walk(struct p9_dev *p9dev,
			   struct p9_pdu *pdu, u32 *outlen)
{
	u8 i;
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_twalk *twalk	= (struct p9_twalk *)outmsg->msg;
	struct p9_rwalk *rwalk	= (struct p9_rwalk *)inmsg->msg;
	struct p9_str *str	= twalk->wnames;
	struct p9_fid *new_fid	= &p9dev->fids[twalk->newfid];


	rwalk->nwqid = 0;
	if (twalk->nwname) {
		struct p9_fid *fid = &p9dev->fids[twalk->fid];

		for (i = 0; i < twalk->nwname; i++) {
			char tmp[PATH_MAX] = {0};
			char full_path[PATH_MAX];
			struct stat st;

			/* Format the new path we're 'walk'ing into */
			sprintf(tmp, "%s/%.*s", fid->path,
				str->len, (char *)&str->str);

			if (lstat(rel_to_abs(p9dev, tmp, full_path), &st) < 0)
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

	*outlen = VIRTIO_P9_HDR_LEN + sizeof(u16) +
		sizeof(struct p9_qid)*rwalk->nwqid;
	set_p9msg_hdr(inmsg, *outlen, P9_RWALK, outmsg->tag);

	return true;
}

static bool virtio_p9_attach(struct p9_dev *p9dev,
			     struct p9_pdu *pdu, u32 *outlen)
{
	u32 i;
	struct stat st;
	struct p9_fid *fid;
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_rattach *rattach = (struct p9_rattach *)inmsg->msg;
	struct p9_tattach *tattach = (struct p9_tattach *)outmsg->msg;

	/* Reset everything */
	for (i = 0; i < VIRTIO_P9_MAX_FID; i++)
		p9dev->fids[i].fid = P9_NOFID;

	if (lstat(p9dev->root_dir, &st) < 0)
		return false;

	st2qid(&st, &rattach->qid);

	fid = &p9dev->fids[tattach->fid];
	fid->fid = tattach->fid;
	fid->is_dir = 1;
	strcpy(fid->path, "/");

	*outlen = VIRTIO_P9_HDR_LEN + sizeof(*rattach);
	set_p9msg_hdr(inmsg, *outlen, P9_RATTACH, outmsg->tag);

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

static bool virtio_p9_read(struct p9_dev *p9dev,
			   struct p9_pdu *pdu, u32 *outlen)
{
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_tread *tread	= (struct p9_tread *)outmsg->msg;
	struct p9_rread *rread	= (struct p9_rread *)inmsg->msg;
	struct p9_rstat *rstat	= (struct p9_rstat *)pdu->in_iov[1].iov_base;
	struct p9_fid *fid	= &p9dev->fids[tread->fid];
	struct stat st;

	rread->count = 0;

	if (fid->is_dir) {
		/* If reading a dir, fill the buffer with p9_stat entries */
		struct dirent *cur = readdir(fid->dir);
		char full_path[PATH_MAX];

		while (cur) {
			u32 read;

			lstat(rel_to_abs(p9dev, cur->d_name, full_path), &st);
			read = virtio_p9_fill_stat(p9dev, cur->d_name,
						   &st, rstat);
			rread->count += read;
			rstat = (void *)rstat + read;
			cur = readdir(fid->dir);
		}
	} else {
		pdu->in_iov[0].iov_base += VIRTIO_P9_HDR_LEN + sizeof(u32);
		pdu->in_iov[0].iov_len -= VIRTIO_P9_HDR_LEN + sizeof(u32);
		pdu->in_iov_cnt = virtio_p9_update_iov_cnt(pdu->in_iov,
							    tread->count,
							    pdu->in_iov_cnt);
		rread->count = preadv(fid->fd, pdu->in_iov,
				      pdu->in_iov_cnt, tread->offset);
		if (rread->count > tread->count)
			rread->count = tread->count;
	}

	*outlen = VIRTIO_P9_HDR_LEN + sizeof(u32) + rread->count;
	set_p9msg_hdr(inmsg, *outlen, P9_RREAD, outmsg->tag);

	return true;
}

static bool virtio_p9_stat(struct p9_dev *p9dev,
			   struct p9_pdu *pdu, u32 *outlen)
{
	u32 ret;
	struct stat st;
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_tstat *tstat = (struct p9_tstat *)outmsg->msg;
	struct p9_rstat *rstat = (struct p9_rstat *)(inmsg->msg + sizeof(u16));
	struct p9_fid *fid = &p9dev->fids[tstat->fid];

	if (lstat(fid->abs_path, &st) < 0)
		return false;

	ret = virtio_p9_fill_stat(p9dev, fid->path, &st, rstat);

	*outlen = VIRTIO_P9_HDR_LEN + ret + sizeof(u16);
	set_p9msg_hdr(inmsg, *outlen, P9_RSTAT, outmsg->tag);
	return true;
}

static bool virtio_p9_wstat(struct p9_dev *p9dev,
			    struct p9_pdu *pdu, u32 *outlen)
{
	int res = 0;
	struct p9_str *str;
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_twstat *twstat = (struct p9_twstat *)outmsg->msg;
	struct p9_fid *fid = &p9dev->fids[twstat->fid];


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
	set_p9msg_hdr(inmsg, *outlen, P9_RWSTAT, outmsg->tag);

	return res == 0;
}

static bool virtio_p9_remove(struct p9_dev *p9dev,
			     struct p9_pdu *pdu, u32 *outlen)
{
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_tremove *tremove = (struct p9_tremove *)outmsg->msg;
	struct p9_fid *fid = &p9dev->fids[tremove->fid];

	close_fid(p9dev, tremove->fid);
	if (fid->is_dir)
		rmdir(fid->abs_path);
	else
		unlink(fid->abs_path);

	*outlen = VIRTIO_P9_HDR_LEN;
	set_p9msg_hdr(inmsg, *outlen, P9_RREMOVE, outmsg->tag);
	return true;
}

static bool virtio_p9_write(struct p9_dev *p9dev,
			    struct p9_pdu *pdu, u32 *outlen)
{
	struct p9_msg *inmsg  = pdu->in_iov[0].iov_base;
	struct p9_msg *outmsg = pdu->out_iov[0].iov_base;
	struct p9_twrite *twrite = (struct p9_twrite *)outmsg->msg;
	struct p9_rwrite *rwrite = (struct p9_rwrite *)inmsg->msg;
	struct p9_fid *fid = &p9dev->fids[twrite->fid];


	pdu->out_iov[0].iov_base += (sizeof(*outmsg) + sizeof(*twrite));
	pdu->out_iov[0].iov_len -= (sizeof(*outmsg) + sizeof(*twrite));
	pdu->out_iov_cnt = virtio_p9_update_iov_cnt(pdu->out_iov, twrite->count,
						    pdu->out_iov_cnt);
	rwrite->count = pwritev(fid->fd, pdu->out_iov,
				pdu->out_iov_cnt, twrite->offset);
	*outlen = VIRTIO_P9_HDR_LEN + sizeof(u32);
	set_p9msg_hdr(inmsg, *outlen, P9_RWRITE, outmsg->tag);

	return true;
}

typedef bool p9_handler(struct p9_dev *p9dev,
			struct p9_pdu *pdu, u32 *outlen);

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

static struct p9_pdu *virtio_p9_pdu_init(struct kvm *kvm, struct virt_queue *vq)
{
	struct p9_pdu *pdu = calloc(1, sizeof(*pdu));
	if (!pdu)
		return NULL;

	pdu->queue_head  = virt_queue__get_inout_iov(kvm, vq, pdu->in_iov,
						     pdu->out_iov,
						     &pdu->in_iov_cnt,
						     &pdu->out_iov_cnt);
	return pdu;
}

static u8 virtio_p9_get_cmd(struct p9_pdu *pdu)
{
	struct p9_msg *msg;
	/*
	 * we can peek directly into pdu for a u8
	 * value. The host endianess won't be an issue
	 */
	msg = pdu->out_iov[0].iov_base;
	return msg->cmd;
}

static bool virtio_p9_do_io_request(struct kvm *kvm, struct p9_dev_job *job)
{
	u8 cmd;
	u32 len = 0;
	p9_handler *handler;
	struct p9_dev *p9dev;
	struct virt_queue *vq;
	struct p9_pdu *p9pdu;

	vq = job->vq;
	p9dev = job->p9dev;

	p9pdu = virtio_p9_pdu_init(kvm, vq);
	cmd = virtio_p9_get_cmd(p9pdu);

	if (cmd >= ARRAY_SIZE(virtio_9p_handler) ||
	    !virtio_9p_handler[cmd]) {
		printf("Unsupported P9 message type: %u\n", cmd);

	} else {
		handler = virtio_9p_handler[cmd];
		handler(p9dev, p9pdu, &len);
	}
	virt_queue__set_used_elem(vq, p9pdu->queue_head, len);
	free(p9pdu);
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

static void ioevent_callback(struct kvm *kvm, void *param)
{
	struct p9_dev_job *job = param;

	thread_pool__do_job(job->job_id);
}

static bool virtio_p9_pci_io_out(struct ioport *ioport, struct kvm *kvm,
				 u16 port, void *data, int size, u32 count)
{
	unsigned long offset;
	bool ret = true;
	struct p9_dev  *p9dev;
	struct ioevent ioevent;

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

		vring_init(&queue->vring, VIRTQUEUE_NUM, p,
			   VIRTIO_PCI_VRING_ALIGN);

		*job			= (struct p9_dev_job) {
			.vq			= queue,
			.p9dev			= p9dev,
		};
		job->job_id = thread_pool__add_job(kvm, virtio_p9_do_io, job);

		ioevent = (struct ioevent) {
			.io_addr		= p9dev->base_addr + VIRTIO_PCI_QUEUE_NOTIFY,
			.io_len			= sizeof(u16),
			.fn			= ioevent_callback,
			.datamatch		= p9dev->queue_selector,
			.fn_ptr			= &p9dev->jobs[p9dev->queue_selector],
			.fn_kvm			= kvm,
			.fd			= eventfd(0, 0),
		};

		ioeventfd__add_event(&ioevent);

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
