#include <libaio.h>
#include <pthread.h>
#include <sys/eventfd.h>

#include "kvm/disk-image.h"
#include "kvm/kvm.h"
#include "linux/list.h"

#define AIO_MAX 256

static int aio_pwritev(io_context_t ctx, struct iocb *iocb, int fd,
		       const struct iovec *iov, int iovcnt, off_t offset,
		       int ev, void *param)
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

static int aio_preadv(io_context_t ctx, struct iocb *iocb, int fd,
		      const struct iovec *iov, int iovcnt, off_t offset,
		      int ev, void *param)
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

ssize_t raw_image__read_async(struct disk_image *disk, u64 sector,
			      const struct iovec *iov, int iovcount,
			      void *param)
{
	u64 offset = sector << SECTOR_SHIFT;
	struct iocb iocb;

	return aio_preadv(disk->ctx, &iocb, disk->fd, iov, iovcount,
			  offset, disk->evt, param);
}

ssize_t raw_image__write_async(struct disk_image *disk, u64 sector,
			       const struct iovec *iov, int iovcount,
			       void *param)
{
	u64 offset = sector << SECTOR_SHIFT;
	struct iocb iocb;

	return aio_pwritev(disk->ctx, &iocb, disk->fd, iov, iovcount,
			   offset, disk->evt, param);
}

static void *disk_aio_thread(void *param)
{
	struct disk_image *disk = param;
	struct io_event event[AIO_MAX];
	struct timespec notime = {0};
	int nr, i;
	u64 dummy;

	kvm__set_thread_name("disk-image-io");

	while (read(disk->evt, &dummy, sizeof(dummy)) > 0) {
		nr = io_getevents(disk->ctx, 1, ARRAY_SIZE(event), event, &notime);
		for (i = 0; i < nr; i++)
			disk->disk_req_cb(event[i].data, event[i].res);
	}

	return NULL;
}

int disk_aio_setup(struct disk_image *disk)
{
	int r;
	pthread_t thread;

	/* No need to setup AIO if the disk ops won't make use of it */
	if (!disk->ops->async)
		return 0;

	disk->evt = eventfd(0, 0);
	if (disk->evt < 0)
		return -errno;

	io_setup(AIO_MAX, &disk->ctx);
	r = pthread_create(&thread, NULL, disk_aio_thread, disk);
	if (r) {
		r = -errno;
		close(disk->evt);
		return r;
	}

	disk->async = true;
	return 0;
}

void disk_aio_destroy(struct disk_image *disk)
{
	if (!disk->async)
		return;

	close(disk->evt);
	io_destroy(disk->ctx);
}
