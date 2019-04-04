#include <libaio.h>
#include <pthread.h>
#include <sys/eventfd.h>

#include "kvm/brlock.h"
#include "kvm/disk-image.h"
#include "kvm/kvm.h"
#include "linux/list.h"

#define AIO_MAX 256

static int aio_submit(struct disk_image *disk, int nr, struct iocb **ios)
{
	int ret;

	__sync_fetch_and_add(&disk->aio_inflight, nr);
	/*
	 * A wmb() is needed here, to ensure disk_aio_thread() sees this
	 * increase after receiving the events. It is included in the
	 * __sync_fetch_and_add (as a full barrier).
	 */
restart:
	ret = io_submit(disk->ctx, nr, ios);
	if (ret == -EAGAIN)
		goto restart;
	else if (ret <= 0)
		/* disk_aio_thread() is never going to see those */
		__sync_fetch_and_sub(&disk->aio_inflight, nr);

	return ret;
}

ssize_t raw_image__read_async(struct disk_image *disk, u64 sector,
			      const struct iovec *iov, int iovcount,
			      void *param)
{
	struct iocb iocb;
	u64 offset = sector << SECTOR_SHIFT;
	struct iocb *ios[1] = { &iocb };

	io_prep_preadv(&iocb, disk->fd, iov, iovcount, offset);
	io_set_eventfd(&iocb, disk->evt);
	iocb.data = param;

	return aio_submit(disk, 1, ios);
}

ssize_t raw_image__write_async(struct disk_image *disk, u64 sector,
			       const struct iovec *iov, int iovcount,
			       void *param)
{
	struct iocb iocb;
	u64 offset = sector << SECTOR_SHIFT;
	struct iocb *ios[1] = { &iocb };

	io_prep_pwritev(&iocb, disk->fd, iov, iovcount, offset);
	io_set_eventfd(&iocb, disk->evt);
	iocb.data = param;

	return aio_submit(disk, 1, ios);
}

/*
 * When this function returns there are no in-flight I/O. Caller ensures that
 * io_submit() isn't called concurrently.
 *
 * Returns an inaccurate number of I/O that was in-flight when the function was
 * called.
 */
int raw_image__wait(struct disk_image *disk)
{
	u64 inflight = disk->aio_inflight;

	while (disk->aio_inflight) {
		usleep(100);
		barrier();
	}

	return inflight;
}

static int disk_aio_get_events(struct disk_image *disk)
{
	struct io_event event[AIO_MAX];
	struct timespec notime = {0};
	int nr, i;

	do {
		nr = io_getevents(disk->ctx, 1, ARRAY_SIZE(event), event, &notime);
		for (i = 0; i < nr; i++)
			disk->disk_req_cb(event[i].data, event[i].res);

		/* Pairs with wmb() in aio_submit() */
		rmb();
		__sync_fetch_and_sub(&disk->aio_inflight, nr);

	} while (nr > 0);

	return 0;
}

static void *disk_aio_thread(void *param)
{
	struct disk_image *disk = param;
	u64 dummy;

	kvm__set_thread_name("disk-image-io");

	while (read(disk->evt, &dummy, sizeof(dummy)) > 0) {
		if (disk_aio_get_events(disk))
			break;
	}

	return NULL;
}

int disk_aio_setup(struct disk_image *disk)
{
	int r;

	/* No need to setup AIO if the disk ops won't make use of it */
	if (!disk->ops->async)
		return 0;

	disk->evt = eventfd(0, 0);
	if (disk->evt < 0)
		return -errno;

	io_setup(AIO_MAX, &disk->ctx);
	r = pthread_create(&disk->thread, NULL, disk_aio_thread, disk);
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

	pthread_cancel(disk->thread);
	pthread_join(disk->thread, NULL);
	close(disk->evt);
	io_destroy(disk->ctx);
}
