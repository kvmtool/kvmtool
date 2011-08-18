#include "kvm/virtio-pci-dev.h"
#include "kvm/ioport.h"
#include "kvm/util.h"
#include "kvm/threadpool.h"
#include "kvm/ioeventfd.h"
#include "kvm/irq.h"
#include "kvm/virtio-9p.h"
#include "kvm/guest_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <linux/virtio_ring.h>
#include <linux/virtio_9p.h>
#include <net/9p/9p.h>

/* Warning: Immediately use value returned from this function */
static const char *rel_to_abs(struct p9_dev *p9dev,
			      const char *path, char *abs_path)
{
	sprintf(abs_path, "%s/%s", p9dev->root_dir, path);

	return abs_path;
}

static bool virtio_p9_dev_in(struct p9_dev *p9dev, void *data,
			     unsigned long offset,
			     int size)
{
	u8 *config_space = (u8 *) p9dev->config;

	if (size != 1)
		return false;

	ioport__write8(data, config_space[offset - VIRTIO_MSI_CONFIG_VECTOR]);

	return true;
}

static bool virtio_p9_pci_io_in(struct ioport *ioport, struct kvm *kvm,
				u16 port, void *data, int size)
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
		ret = virtio_p9_dev_in(p9dev, data, offset, size);
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

static void virtio_p9_set_reply_header(struct p9_pdu *pdu, u32 size)
{
	u8 cmd;
	u16 tag;

	pdu->read_offset = sizeof(u32);
	virtio_p9_pdu_readf(pdu, "bw", &cmd, &tag);
	pdu->write_offset = 0;
	/* cmd + 1 is the reply message */
	virtio_p9_pdu_writef(pdu, "dbw", size, cmd + 1, tag);
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
	u16 tag;
	char *err_str;

	err_str = strerror(err);
	pdu->write_offset = VIRTIO_P9_HDR_LEN;
	virtio_p9_pdu_writef(pdu, "sd", err_str, err);
	*outlen = pdu->write_offset;

	pdu->read_offset = sizeof(u32) + sizeof(u8);
	virtio_p9_pdu_readf(pdu, "w", &tag);

	pdu->write_offset = 0;
	virtio_p9_pdu_writef(pdu, "dbw", *outlen, P9_RERROR, tag);
}

static void virtio_p9_version(struct p9_dev *p9dev,
			      struct p9_pdu *pdu, u32 *outlen)
{
	virtio_p9_pdu_writef(pdu, "ds", 4096, VIRTIO_P9_VERSION);

	*outlen = pdu->write_offset;
	virtio_p9_set_reply_header(pdu, *outlen);
	return;
}

static void virtio_p9_clunk(struct p9_dev *p9dev,
			    struct p9_pdu *pdu, u32 *outlen)
{
	u32 fid;

	virtio_p9_pdu_readf(pdu, "d", &fid);
	close_fid(p9dev, fid);

	*outlen = pdu->write_offset;
	virtio_p9_set_reply_header(pdu, *outlen);
	return;
}

static void virtio_p9_open(struct p9_dev *p9dev,
			   struct p9_pdu *pdu, u32 *outlen)
{
	u8 mode;
	u32 fid;
	struct stat st;
	struct p9_qid qid;
	struct p9_fid *new_fid;

	virtio_p9_pdu_readf(pdu, "db", &fid, &mode);
	new_fid = &p9dev->fids[fid];

	if (lstat(new_fid->abs_path, &st) < 0)
		goto err_out;

	st2qid(&st, &qid);

	if (new_fid->is_dir) {
		new_fid->dir = opendir(new_fid->abs_path);
		if (!new_fid->dir)
			goto err_out;
	} else {
		new_fid->fd  = open(new_fid->abs_path,
				    omode2uflags(mode) | O_NOFOLLOW);
		if (new_fid->fd < 0)
			goto err_out;
	}
	virtio_p9_pdu_writef(pdu, "Qd", &qid, 0);

	*outlen = pdu->write_offset;
	virtio_p9_set_reply_header(pdu, *outlen);
	return;
err_out:
	virtio_p9_error_reply(p9dev, pdu, errno, outlen);
	return;
}

static void virtio_p9_create(struct p9_dev *p9dev,
			     struct p9_pdu *pdu, u32 *outlen)
{
	DIR *dir;
	int fd;
	int res;
	u8 mode;
	u32 perm;
	char *name;
	char *ext = NULL;
	u32 fid_val;
	struct stat st;
	struct p9_qid qid;
	struct p9_fid *fid;
	char full_path[PATH_MAX];

	virtio_p9_pdu_readf(pdu, "dsdbs", &fid_val, &name, &perm, &mode, &ext);
	fid = &p9dev->fids[fid_val];

	sprintf(full_path, "%s/%s", fid->abs_path, name);
	if (perm & P9_DMDIR) {
		res = mkdir(full_path, perm & 0xFFFF);
		if (res < 0)
			goto err_out;
		dir = opendir(full_path);
		if (!dir)
			goto err_out;
		close_fid(p9dev, fid_val);
		fid->dir = dir;
		fid->is_dir = 1;
	} else if (perm & P9_DMSYMLINK) {
		if (symlink(ext, full_path) < 0)
			goto err_out;
	} else if (perm & P9_DMLINK) {
		int ext_fid = atoi(ext);

		if (link(p9dev->fids[ext_fid].abs_path, full_path) < 0)
			goto err_out;
	} else {
		fd = open(full_path, omode2uflags(mode) | O_CREAT, 0777);
		if (fd < 0)
			goto err_out;
		close_fid(p9dev, fid_val);
		fid->fd = fd;
	}
	if (lstat(full_path, &st) < 0)
		goto err_out;

	sprintf(fid->path, "%s/%s", fid->path, name);
	st2qid(&st, &qid);
	virtio_p9_pdu_writef(pdu, "Qd", &qid, 0);
	*outlen = pdu->write_offset;
	virtio_p9_set_reply_header(pdu, *outlen);

	free(name);
	free(ext);
	return;
err_out:
	virtio_p9_error_reply(p9dev, pdu, errno, outlen);
	free(name);
	free(ext);
	return;
}

static void virtio_p9_walk(struct p9_dev *p9dev,
			   struct p9_pdu *pdu, u32 *outlen)
{
	u8 i;
	u16 nwqid;
	char *str;
	u16 nwname;
	u32 fid_val;
	u32 newfid_val;
	struct p9_qid wqid;
	struct p9_fid *new_fid;
	int ret;

	virtio_p9_pdu_readf(pdu, "ddw", &fid_val, &newfid_val, &nwname);
	new_fid	= &p9dev->fids[newfid_val];

	nwqid = 0;
	if (nwname) {
		struct p9_fid *fid = &p9dev->fids[fid_val];

		/* skip the space for count */
		pdu->write_offset += sizeof(u16);
		for (i = 0; i < nwname; i++) {
			struct stat st;
			char tmp[PATH_MAX] = {0};
			char full_path[PATH_MAX];

			virtio_p9_pdu_readf(pdu, "s", &str);

			/* Format the new path we're 'walk'ing into */
			sprintf(tmp, "%s/%.*s",
				fid->path, (int)strlen(str), str);

			ret = lstat(rel_to_abs(p9dev, tmp, full_path), &st);
			if (ret < 0)
				goto err_out;

			st2qid(&st, &wqid);
			new_fid->is_dir = S_ISDIR(st.st_mode);
			strcpy(new_fid->path, tmp);
			new_fid->fid = newfid_val;
			nwqid++;
			virtio_p9_pdu_writef(pdu, "Q", &wqid);
		}
	} else {
		/*
		 * update write_offset so our outlen get correct value
		 */
		pdu->write_offset += sizeof(u16);
		new_fid->is_dir = p9dev->fids[fid_val].is_dir;
		strcpy(new_fid->path, p9dev->fids[fid_val].path);
		new_fid->fid	= newfid_val;
	}
	*outlen = pdu->write_offset;
	pdu->write_offset = VIRTIO_P9_HDR_LEN;
	virtio_p9_pdu_writef(pdu, "d", nwqid);
	virtio_p9_set_reply_header(pdu, *outlen);
	return;
err_out:
	virtio_p9_error_reply(p9dev, pdu, errno, outlen);
	return;
}

static void virtio_p9_attach(struct p9_dev *p9dev,
			     struct p9_pdu *pdu, u32 *outlen)
{
	u32 i;
	u32 fid_val;
	u32 afid;
	char *uname;
	char *aname;
	struct stat st;
	struct p9_qid qid;
	struct p9_fid *fid;

	virtio_p9_pdu_readf(pdu, "ddss", &fid_val, &afid, &uname, &aname);

	/* Reset everything */
	for (i = 0; i < VIRTIO_P9_MAX_FID; i++)
		p9dev->fids[i].fid = P9_NOFID;

	if (lstat(p9dev->root_dir, &st) < 0)
		goto err_out;

	st2qid(&st, &qid);

	fid = &p9dev->fids[fid_val];
	fid->fid = fid_val;
	fid->is_dir = 1;
	strcpy(fid->path, "/");

	virtio_p9_pdu_writef(pdu, "Q", &qid);
	*outlen = pdu->write_offset;
	virtio_p9_set_reply_header(pdu, *outlen);
	free(uname);
	free(aname);
	return;
err_out:
	free(uname);
	free(aname);
	virtio_p9_error_reply(p9dev, pdu, errno, outlen);
	return;
}

static void virtio_p9_free_stat(struct p9_wstat *wstat)
{
	free(wstat->extension);
	free(wstat->name);
}

static void virtio_p9_fill_stat(struct p9_dev *p9dev, const char *name,
				struct stat *st, struct p9_wstat *wstat)
{
	wstat->type = 0;
	wstat->dev = 0;
	st2qid(st, &wstat->qid);
	wstat->mode = st->st_mode;
	wstat->length = st->st_size;
	if (S_ISDIR(st->st_mode)) {
		wstat->length = 0;
		wstat->mode |= P9_DMDIR;
	}
	if (S_ISLNK(st->st_mode)) {
		char tmp[PATH_MAX] = {0}, full_path[PATH_MAX] = {0};

		rel_to_abs(p9dev, name, full_path);

		if (readlink(full_path, tmp, PATH_MAX) > 0)
			wstat->extension = strdup(tmp);
		wstat->mode |= P9_DMSYMLINK;
	} else {
		wstat->extension = NULL;
	}

	wstat->atime = st->st_atime;
	wstat->mtime = st->st_mtime;

	wstat->name = strdup(name);
	wstat->uid = NULL;
	wstat->gid = NULL;
	wstat->muid = NULL;
	wstat->n_uid = wstat->n_gid = wstat->n_muid = 0;

	/*
	 * NOTE: size shouldn't include its own length
	 * size[2] type[2] dev[4] qid[13]
	 * mode[4] atime[4] mtime[4] length[8]
	 * name[s] uid[s] gid[s] muid[s]
	 * ext[s] uid[4] gid[4] muid[4]
	 */
	wstat->size = 2+4+13+4+4+4+8+2+2+2+2+2+4+4+4;
	if (wstat->name)
		wstat->size += strlen(wstat->name);
	if (wstat->extension)
		wstat->size += strlen(wstat->extension);
}

static void virtio_p9_read(struct p9_dev *p9dev,
			   struct p9_pdu *pdu, u32 *outlen)
{
	u64 offset;
	u32 fid_val;
	u32 count, rcount;
	struct stat st;
	struct p9_fid *fid;
	struct p9_wstat wstat;

	rcount = 0;
	virtio_p9_pdu_readf(pdu, "dqd", &fid_val, &offset, &count);
	fid = &p9dev->fids[fid_val];
	if (fid->is_dir) {
		/* If reading a dir, fill the buffer with p9_stat entries */
		char full_path[PATH_MAX];
		struct dirent *cur = readdir(fid->dir);

		/* Skip the space for writing count */
		pdu->write_offset += sizeof(u32);
		while (cur) {
			u32 read;

			lstat(rel_to_abs(p9dev, cur->d_name, full_path), &st);
			virtio_p9_fill_stat(p9dev, cur->d_name, &st, &wstat);

			read = pdu->write_offset;
			virtio_p9_pdu_writef(pdu, "S", &wstat);
			rcount += pdu->write_offset - read;
			virtio_p9_free_stat(&wstat);

			cur = readdir(fid->dir);
		}
	} else {
		u16 iov_cnt;
		void *iov_base;
		size_t iov_len;

		iov_base = pdu->in_iov[0].iov_base;
		iov_len  = pdu->in_iov[0].iov_len;
		iov_cnt  = pdu->in_iov_cnt;

		pdu->in_iov[0].iov_base += VIRTIO_P9_HDR_LEN + sizeof(u32);
		pdu->in_iov[0].iov_len -= VIRTIO_P9_HDR_LEN + sizeof(u32);
		pdu->in_iov_cnt = virtio_p9_update_iov_cnt(pdu->in_iov,
							   count,
							   pdu->in_iov_cnt);
		rcount = preadv(fid->fd, pdu->in_iov,
				pdu->in_iov_cnt, offset);
		if (rcount > count)
			rcount = count;
		/*
		 * Update the iov_base back, so that rest of
		 * pdu_writef works correctly.
		 */
		pdu->in_iov[0].iov_base = iov_base;
		pdu->in_iov[0].iov_len  = iov_len;
		pdu->in_iov_cnt         = iov_cnt;
	}
	pdu->write_offset = VIRTIO_P9_HDR_LEN;
	virtio_p9_pdu_writef(pdu, "d", rcount);
	*outlen = pdu->write_offset + rcount;
	virtio_p9_set_reply_header(pdu, *outlen);

	return;
}

static void virtio_p9_stat(struct p9_dev *p9dev,
			   struct p9_pdu *pdu, u32 *outlen)
{
	u32 fid_val;
	struct stat st;
	struct p9_fid *fid;
	struct p9_wstat wstat;

	virtio_p9_pdu_readf(pdu, "d", &fid_val);
	fid = &p9dev->fids[fid_val];
	if (lstat(fid->abs_path, &st) < 0)
		goto err_out;

	virtio_p9_fill_stat(p9dev, fid->path, &st, &wstat);

	virtio_p9_pdu_writef(pdu, "wS", 0, &wstat);
	*outlen = pdu->write_offset;
	virtio_p9_free_stat(&wstat);
	virtio_p9_set_reply_header(pdu, *outlen);
	return;
err_out:
	virtio_p9_error_reply(p9dev, pdu, errno, outlen);
	return;
}

static void virtio_p9_wstat(struct p9_dev *p9dev,
			    struct p9_pdu *pdu, u32 *outlen)
{
	int res = 0;
	u32 fid_val;
	u16 unused;
	struct p9_fid *fid;
	struct p9_wstat wstat;

	virtio_p9_pdu_readf(pdu, "dwS", &fid_val, &unused, &wstat);
	fid = &p9dev->fids[fid_val];

	if (wstat.length != -1UL) {
		res = truncate(fid->abs_path, wstat.length);
		if (res < 0)
			goto err_out;
	}
	if (wstat.mode != -1U) {
		res = chmod(fid->abs_path, wstat.mode & 0xFFFF);
		if (res < 0)
			goto err_out;
	}
	if (strlen(wstat.name) > 0) {
		char new_name[PATH_MAX] = {0};
		char full_path[PATH_MAX];
		char *last_dir = strrchr(fid->path, '/');

		/* We need to get the full file name out of twstat->name */
		if (last_dir)
			strncpy(new_name, fid->path, last_dir - fid->path + 1);

		memcpy(new_name + strlen(new_name),
		       wstat.name, strlen(wstat.name));

		/* fid is reused for the new file */
		res = rename(fid->abs_path,
			     rel_to_abs(p9dev, new_name, full_path));
		if (res < 0)
			goto err_out;
		sprintf(fid->path, "%s", new_name);
	}
	*outlen = VIRTIO_P9_HDR_LEN;
	virtio_p9_set_reply_header(pdu, *outlen);
	return;
err_out:
	virtio_p9_error_reply(p9dev, pdu, errno, outlen);
	return;
}

static void virtio_p9_remove(struct p9_dev *p9dev,
			     struct p9_pdu *pdu, u32 *outlen)
{
	int res;
	u32 fid_val;
	struct p9_fid *fid;

	virtio_p9_pdu_readf(pdu, "d", &fid_val);
	fid = &p9dev->fids[fid_val];
	close_fid(p9dev, fid_val);
	if (fid->is_dir)
		res = rmdir(fid->abs_path);
	else
		res = unlink(fid->abs_path);
	if (res < 0)
		goto err_out;

	*outlen = VIRTIO_P9_HDR_LEN;
	virtio_p9_set_reply_header(pdu, *outlen);
	return;
err_out:
	virtio_p9_error_reply(p9dev, pdu, errno, outlen);
	return;
}

static void virtio_p9_write(struct p9_dev *p9dev,
			    struct p9_pdu *pdu, u32 *outlen)
{

	u64 offset;
	u32 fid_val;
	u32 count;
	ssize_t res;
	u16 iov_cnt;
	void *iov_base;
	size_t iov_len;
	struct p9_fid *fid;
	/* u32 fid + u64 offset + u32 count */
	int twrite_size = sizeof(u32) + sizeof(u64) + sizeof(u32);

	virtio_p9_pdu_readf(pdu, "dqd", &fid_val, &offset, &count);
	fid = &p9dev->fids[fid_val];

	iov_base = pdu->out_iov[0].iov_base;
	iov_len  = pdu->out_iov[0].iov_len;
	iov_cnt  = pdu->out_iov_cnt;

	/* Adjust the iovec to skip the header and meta data */
	pdu->out_iov[0].iov_base += (sizeof(struct p9_msg) + twrite_size);
	pdu->out_iov[0].iov_len -=  (sizeof(struct p9_msg) + twrite_size);
	pdu->out_iov_cnt = virtio_p9_update_iov_cnt(pdu->out_iov, count,
						    pdu->out_iov_cnt);
	res = pwritev(fid->fd, pdu->out_iov, pdu->out_iov_cnt, offset);
	/*
	 * Update the iov_base back, so that rest of
	 * pdu_readf works correctly.
	 */
	pdu->out_iov[0].iov_base = iov_base;
	pdu->out_iov[0].iov_len  = iov_len;
	pdu->out_iov_cnt         = iov_cnt;
	if (res < 0)
		goto err_out;
	virtio_p9_pdu_writef(pdu, "d", res);
	*outlen = pdu->write_offset;
	virtio_p9_set_reply_header(pdu, *outlen);
	return;
err_out:
	virtio_p9_error_reply(p9dev, pdu, errno, outlen);
	return;
}

typedef void p9_handler(struct p9_dev *p9dev,
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

	/* skip the pdu header p9_msg */
	pdu->read_offset  = VIRTIO_P9_HDR_LEN;
	pdu->write_offset = VIRTIO_P9_HDR_LEN;
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

static void virtio_p9_eopnotsupp(struct p9_dev *p9dev,
				 struct p9_pdu *pdu, u32 *outlen)
{
	return virtio_p9_error_reply(p9dev, pdu, EOPNOTSUPP, outlen);
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
	    !virtio_9p_handler[cmd])
		handler = virtio_p9_eopnotsupp;
	else
		handler = virtio_9p_handler[cmd];
	handler(p9dev, p9pdu, &len);
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

	thread_pool__do_job(&job->job_id);
}

static bool virtio_p9_pci_io_out(struct ioport *ioport, struct kvm *kvm,
				 u16 port, void *data, int size)
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

		compat__remove_message(p9dev->compat_id);

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
		thread_pool__init_job(&job->job_id, kvm, virtio_p9_do_io, job);

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
		thread_pool__do_job(&p9dev->jobs[queue_index].job_id);
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

int virtio_9p__init(struct kvm *kvm, const char *root, const char *tag_name)
{
	struct p9_dev *p9dev;
	u8 pin, line, dev;
	u16 p9_base_addr;
	u32 i, root_len;
	int err = 0;

	p9dev = calloc(1, sizeof(*p9dev));
	if (!p9dev)
		return -ENOMEM;

	if (!tag_name)
		tag_name = VIRTIO_P9_DEFAULT_TAG;

	p9dev->config = calloc(1, sizeof(*p9dev->config) + strlen(tag_name) + 1);
	if (p9dev->config == NULL) {
		err = -ENOMEM;
		goto free_p9dev;
	}

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
	if (p9dev->config->tag_len > MAX_TAG_LEN) {
		err = -EINVAL;
		goto free_p9dev_config;
	}

	memcpy(p9dev->config->tag, tag_name, strlen(tag_name));
	p9dev->features |= 1 << VIRTIO_9P_MOUNT_TAG;

	err = irq__register_device(VIRTIO_ID_9P, &dev, &pin, &line);
	if (err < 0)
		goto free_p9dev_config;

	p9_base_addr = ioport__register(IOPORT_EMPTY, &virtio_p9_io_ops, IOPORT_SIZE, p9dev);

	p9dev->base_addr = p9_base_addr;

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

	p9dev->compat_id = compat__add_message("virtio-9p device was not detected",
						"While you have requested a virtio-9p device, "
						"the guest kernel didn't seem to detect it.\n"
						"Please make sure that the kernel was compiled"
						"with CONFIG_NET_9P_VIRTIO.");

	return err;

free_p9dev_config:
	free(p9dev->config);
free_p9dev:
	free(p9dev);
	return err;
}
