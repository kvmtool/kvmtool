#include "kvm/read-write.h"

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* Same as read(2) except that this function never returns EAGAIN or EINTR. */
ssize_t xread(int fd, void *buf, size_t count)
{
	ssize_t nr;

restart:
	nr = read(fd, buf, count);
	if ((nr < 0) && ((errno == EAGAIN) || (errno == EINTR)))
		goto restart;

	return nr;
}

/* Same as write(2) except that this function never returns EAGAIN or EINTR. */
ssize_t xwrite(int fd, const void *buf, size_t count)
{
	ssize_t nr;

restart:
	nr = write(fd, buf, count);
	if ((nr < 0) && ((errno == EAGAIN) || (errno == EINTR)))
		goto restart;

	return nr;
}

ssize_t read_in_full(int fd, void *buf, size_t count)
{
	ssize_t total = 0;
	char *p = buf;

	while (count > 0) {
		ssize_t nr;

		nr = xread(fd, p, count);
		if (nr <= 0) {
			if (total > 0)
				return total;

			return -1;
		}

		count -= nr;
		total += nr;
		p += nr;
	}

	return total;
}

ssize_t write_in_full(int fd, const void *buf, size_t count)
{
	const char *p = buf;
	ssize_t total = 0;

	while (count > 0) {
		ssize_t nr;

		nr = xwrite(fd, p, count);
		if (nr < 0)
			return -1;
		if (nr == 0) {
			errno = ENOSPC;
			return -1;
		}
		count -= nr;
		total += nr;
		p += nr;
	}

	return total;
}

/* Same as pread(2) except that this function never returns EAGAIN or EINTR. */
ssize_t xpread(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t nr;

restart:
	nr = pread(fd, buf, count, offset);
	if ((nr < 0) && ((errno == EAGAIN) || (errno == EINTR)))
		goto restart;

	return nr;
}

/* Same as pwrite(2) except that this function never returns EAGAIN or EINTR. */
ssize_t xpwrite(int fd, const void *buf, size_t count, off_t offset)
{
	ssize_t nr;

restart:
	nr = pwrite(fd, buf, count, offset);
	if ((nr < 0) && ((errno == EAGAIN) || (errno == EINTR)))
		goto restart;

	return nr;
}

ssize_t pread_in_full(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t total = 0;
	char *p = buf;

	while (count > 0) {
		ssize_t nr;

		nr = xpread(fd, p, count, offset);
		if (nr <= 0) {
			if (total > 0)
				return total;

			return -1;
		}

		count -= nr;
		total += nr;
		p += nr;
		offset += nr;
	}

	return total;
}

ssize_t pwrite_in_full(int fd, const void *buf, size_t count, off_t offset)
{
	const char *p = buf;
	ssize_t total = 0;

	while (count > 0) {
		ssize_t nr;

		nr = xpwrite(fd, p, count, offset);
		if (nr < 0)
			return -1;
		if (nr == 0) {
			errno = ENOSPC;
			return -1;
		}
		count -= nr;
		total += nr;
		p += nr;
		offset += nr;
	}

	return total;
}

/* Same as readv(2) except that this function never returns EAGAIN or EINTR. */
ssize_t xreadv(int fd, const struct iovec *iov, int iovcnt)
{
	ssize_t nr;

restart:
	nr = readv(fd, iov, iovcnt);
	if ((nr < 0) && ((errno == EAGAIN) || (errno == EINTR)))
		goto restart;

	return nr;
}

/* Same as writev(2) except that this function never returns EAGAIN or EINTR. */
ssize_t xwritev(int fd, const struct iovec *iov, int iovcnt)
{
	ssize_t nr;

restart:
	nr = writev(fd, iov, iovcnt);
	if ((nr < 0) && ((errno == EAGAIN) || (errno == EINTR)))
		goto restart;

	return nr;
}

static inline ssize_t get_iov_size(const struct iovec *iov, int iovcnt)
{
	size_t size = 0;
	while (iovcnt--)
		size += (iov++)->iov_len;

	return size;
}

static inline void shift_iovec(const struct iovec **iov, int *iovcnt,
				size_t nr, ssize_t *total, size_t *count, off_t *offset)
{
	while (nr >= (*iov)->iov_len) {
		nr -= (*iov)->iov_len;
		*total += (*iov)->iov_len;
		*count -= (*iov)->iov_len;
		if (offset)
			*offset += (*iov)->iov_len;
		(*iovcnt)--;
		(*iov)++;
	}
}

ssize_t readv_in_full(int fd, const struct iovec *iov, int iovcnt)
{
	ssize_t total = 0;
	size_t count = get_iov_size(iov, iovcnt);

	while (count > 0) {
		ssize_t nr;

		nr = xreadv(fd, iov, iovcnt);
		if (nr <= 0) {
			if (total > 0)
				return total;

			return -1;
		}

		shift_iovec(&iov, &iovcnt, nr, &total, &count, NULL);
	}

	return total;
}

ssize_t writev_in_full(int fd, const struct iovec *iov, int iovcnt)
{
	ssize_t total = 0;
	size_t count = get_iov_size(iov, iovcnt);

	while (count > 0) {
		ssize_t nr;

		nr = xwritev(fd, iov, iovcnt);
		if (nr < 0)
			return -1;
		if (nr == 0) {
			errno = ENOSPC;
			return -1;
		}

		shift_iovec(&iov, &iovcnt, nr, &total, &count, NULL);
	}

	return total;
}

/* Same as preadv(2) except that this function never returns EAGAIN or EINTR. */
ssize_t xpreadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t nr;

restart:
	nr = preadv(fd, iov, iovcnt, offset);
	if ((nr < 0) && ((errno == EAGAIN) || (errno == EINTR)))
		goto restart;

	return nr;
}

/* Same as pwritev(2) except that this function never returns EAGAIN or EINTR. */
ssize_t xpwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t nr;

restart:
	nr = pwritev(fd, iov, iovcnt, offset);
	if ((nr < 0) && ((errno == EAGAIN) || (errno == EINTR)))
		goto restart;

	return nr;
}

ssize_t preadv_in_full(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t total = 0;
	size_t count = get_iov_size(iov, iovcnt);

	while (count > 0) {
		ssize_t nr;

		nr = xpreadv(fd, iov, iovcnt, offset);
		if (nr <= 0) {
			if (total > 0)
				return total;

			return -1;
		}

		shift_iovec(&iov, &iovcnt, nr, &total, &count, &offset);
	}

	return total;
}

ssize_t pwritev_in_full(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	ssize_t total = 0;
	size_t count = get_iov_size(iov, iovcnt);

	while (count > 0) {
		ssize_t nr;

		nr = xpwritev(fd, iov, iovcnt, offset);
		if (nr < 0)
			return -1;
		if (nr == 0) {
			errno = ENOSPC;
			return -1;
		}

		shift_iovec(&iov, &iovcnt, nr, &total, &count, &offset);
	}

	return total;
}

#ifdef CONFIG_HAS_AIO
int aio_pwritev(io_context_t ctx, struct iocb *iocb, int fd, const struct iovec *iov, int iovcnt,
		off_t offset, int ev, void *param)
{
	struct iocb *ios[1] = { iocb };
	int ret;

	io_prep_pwritev(iocb, fd, iov, iovcnt, offset);
	io_set_eventfd(iocb, ev);
	iocb->data = param;

restart:
	ret = io_submit(ctx, 1, ios);
	if (ret == -EAGAIN)
		goto restart;
	return ret;
}

int aio_preadv(io_context_t ctx, struct iocb *iocb, int fd, const struct iovec *iov, int iovcnt,
		off_t offset, int ev, void *param)
{
	struct iocb *ios[1] = { iocb };
	int ret;

	io_prep_preadv(iocb, fd, iov, iovcnt, offset);
	io_set_eventfd(iocb, ev);
	iocb->data = param;

restart:
	ret = io_submit(ctx, 1, ios);
	if (ret == -EAGAIN)
		goto restart;
	return ret;
}
#endif
