#include "kvm/util.h"
#include "kvm/virtio-9p.h"

#include <endian.h>
#include <stdint.h>

#include <linux/compiler.h>
#include <net/9p/9p.h>

static void virtio_p9_pdu_read(struct p9_pdu *pdu, void *data, size_t size)
{
	size_t len;
	int i, copied = 0;
	u16 iov_cnt = pdu->out_iov_cnt;
	size_t offset = pdu->read_offset;
	struct iovec *iov = pdu->out_iov;

	for (i = 0; i < iov_cnt && size; i++) {
		if (offset >= iov[i].iov_len) {
			offset -= iov[i].iov_len;
			continue;
		} else {
			len = MIN(iov[i].iov_len - offset, size);
			memcpy(data, iov[i].iov_base + offset, len);
			size -= len;
			data += len;
			offset = 0;
			copied += len;
		}
	}
	pdu->read_offset += copied;
}

static void virtio_p9_pdu_write(struct p9_pdu *pdu,
				const void *data, size_t size)
{
	size_t len;
	int i, copied = 0;
	u16 iov_cnt = pdu->in_iov_cnt;
	size_t offset = pdu->write_offset;
	struct iovec *iov = pdu->in_iov;

	for (i = 0; i < iov_cnt && size; i++) {
		if (offset >= iov[i].iov_len) {
			offset -= iov[i].iov_len;
			continue;
		} else {
			len = MIN(iov[i].iov_len - offset, size);
			memcpy(iov[i].iov_base + offset, data, len);
			size -= len;
			data += len;
			offset = 0;
			copied += len;
		}
	}
	pdu->write_offset += copied;
}

static void virtio_p9_wstat_free(struct p9_wstat *stbuf)
{
	free(stbuf->name);
	free(stbuf->uid);
	free(stbuf->gid);
	free(stbuf->muid);
}

static int virtio_p9_decode(struct p9_pdu *pdu, const char *fmt, va_list ap)
{
	int retval = 0;
	const char *ptr;

	for (ptr = fmt; *ptr; ptr++) {
		switch (*ptr) {
		case 'b':
		{
			int8_t *val = va_arg(ap, int8_t *);
			virtio_p9_pdu_read(pdu, val, sizeof(*val));
		}
		break;
		case 'w':
		{
			int16_t le_val;
			int16_t *val = va_arg(ap, int16_t *);
			virtio_p9_pdu_read(pdu, &le_val, sizeof(le_val));
			*val = le16toh(le_val);
		}
		break;
		case 'd':
		{
			int32_t le_val;
			int32_t *val = va_arg(ap, int32_t *);
			virtio_p9_pdu_read(pdu, &le_val, sizeof(le_val));
			*val = le32toh(le_val);
		}
		break;
		case 'q':
		{
			int64_t le_val;
			int64_t *val = va_arg(ap, int64_t *);
			virtio_p9_pdu_read(pdu, &le_val, sizeof(le_val));
			*val = le64toh(le_val);
		}
		break;
		case 's':
		{
			int16_t len;
			char **str = va_arg(ap, char **);

			virtio_p9_pdu_readf(pdu, "w", &len);
			*str = malloc(len + 1);
			if (*str == NULL) {
				retval = ENOMEM;
				break;
			}
			virtio_p9_pdu_read(pdu, *str, len);
			(*str)[len] = 0;
		}
		break;
		case 'Q':
		{
			struct p9_qid *qid = va_arg(ap, struct p9_qid *);
			retval = virtio_p9_pdu_readf(pdu, "bdq",
						     &qid->type, &qid->version,
						     &qid->path);
		}
		break;
		case 'S':
		{
			struct p9_wstat *stbuf = va_arg(ap, struct p9_wstat *);
			memset(stbuf, 0, sizeof(struct p9_wstat));
			stbuf->n_uid = stbuf->n_gid = stbuf->n_muid = -1;
			retval = virtio_p9_pdu_readf(pdu, "wwdQdddqssss",
						&stbuf->size, &stbuf->type,
						&stbuf->dev, &stbuf->qid,
						&stbuf->mode, &stbuf->atime,
						&stbuf->mtime, &stbuf->length,
						&stbuf->name, &stbuf->uid,
						&stbuf->gid, &stbuf->muid);
			if (retval)
				virtio_p9_wstat_free(stbuf);
		}
		break;
		case 'I':
		{
			struct p9_iattr_dotl *p9attr = va_arg(ap,
						       struct p9_iattr_dotl *);

			retval = virtio_p9_pdu_readf(pdu, "ddddqqqqq",
						     &p9attr->valid,
						     &p9attr->mode,
						     &p9attr->uid,
						     &p9attr->gid,
						     &p9attr->size,
						     &p9attr->atime_sec,
						     &p9attr->atime_nsec,
						     &p9attr->mtime_sec,
						     &p9attr->mtime_nsec);
		}
		break;
		default:
			retval = EINVAL;
			break;
		}
	}
	return retval;
}

static int virtio_p9_pdu_encode(struct p9_pdu *pdu, const char *fmt, va_list ap)
{
	int retval = 0;
	const char *ptr;

	for (ptr = fmt; *ptr; ptr++) {
		switch (*ptr) {
		case 'b':
		{
			int8_t val = va_arg(ap, int);
			virtio_p9_pdu_write(pdu, &val, sizeof(val));
		}
		break;
		case 'w':
		{
			int16_t val = htole16(va_arg(ap, int));
			virtio_p9_pdu_write(pdu, &val, sizeof(val));
		}
		break;
		case 'd':
		{
			int32_t val = htole32(va_arg(ap, int32_t));
			virtio_p9_pdu_write(pdu, &val, sizeof(val));
		}
		break;
		case 'q':
		{
			int64_t val = htole64(va_arg(ap, int64_t));
			virtio_p9_pdu_write(pdu, &val, sizeof(val));
		}
		break;
		case 's':
		{
			uint16_t len = 0;
			const char *s = va_arg(ap, char *);
			if (s)
				len = MIN(strlen(s), USHRT_MAX);
			virtio_p9_pdu_writef(pdu, "w", len);
			virtio_p9_pdu_write(pdu, s, len);
		}
		break;
		case 'Q':
		{
			struct p9_qid *qid = va_arg(ap, struct p9_qid *);
			retval = virtio_p9_pdu_writef(pdu, "bdq",
						      qid->type, qid->version,
						      qid->path);
		}
		break;
		case 'S':
		{
			struct p9_wstat *stbuf = va_arg(ap, struct p9_wstat *);
			retval = virtio_p9_pdu_writef(pdu, "wwdQdddqssss",
						stbuf->size, stbuf->type,
						stbuf->dev, &stbuf->qid,
						stbuf->mode, stbuf->atime,
						stbuf->mtime, stbuf->length,
						stbuf->name, stbuf->uid,
						stbuf->gid, stbuf->muid);
		}
		break;
		case 'A':
		{
			struct p9_stat_dotl *stbuf = va_arg(ap,
						      struct p9_stat_dotl *);
			retval  = virtio_p9_pdu_writef(pdu,
						       "qQdddqqqqqqqqqqqqqqq",
						       stbuf->st_result_mask,
						       &stbuf->qid,
						       stbuf->st_mode,
						       stbuf->st_uid,
						       stbuf->st_gid,
						       stbuf->st_nlink,
						       stbuf->st_rdev,
						       stbuf->st_size,
						       stbuf->st_blksize,
						       stbuf->st_blocks,
						       stbuf->st_atime_sec,
						       stbuf->st_atime_nsec,
						       stbuf->st_mtime_sec,
						       stbuf->st_mtime_nsec,
						       stbuf->st_ctime_sec,
						       stbuf->st_ctime_nsec,
						       stbuf->st_btime_sec,
						       stbuf->st_btime_nsec,
						       stbuf->st_gen,
						       stbuf->st_data_version);
		}
		break;
		default:
			retval = EINVAL;
			break;
		}
	}
	return retval;
}

int virtio_p9_pdu_readf(struct p9_pdu *pdu, const char *fmt, ...)
{
	int ret;
	va_list ap;

	va_start(ap, fmt);
	ret = virtio_p9_decode(pdu, fmt, ap);
	va_end(ap);

	return ret;
}

int virtio_p9_pdu_writef(struct p9_pdu *pdu, const char *fmt, ...)
{
	int ret;
	va_list ap;

	va_start(ap, fmt);
	ret = virtio_p9_pdu_encode(pdu, fmt, ap);
	va_end(ap);

	return ret;
}
