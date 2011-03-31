#include "kvm/disk-image.h"

#include "kvm/read-write.h"
#include "kvm/util.h"

#include <sys/types.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

struct disk_image *disk_image__open(const char *filename)
{
	struct disk_image *self;
	struct stat st;

	self		= malloc(sizeof *self);
	if (!self)
		return NULL;

	self->fd	= open(filename, O_RDWR);
	if (self->fd < 0)
		goto failed_free;

	if (fstat(self->fd, &st) < 0)
		goto failed_close_fd;

	self->size	= st.st_size;

	return self;

failed_close_fd:
	close(self->fd);
failed_free:
	free(self);

	return NULL;
}

void disk_image__close(struct disk_image *self)
{
	if (close(self->fd) < 0)
		warning("close() failed");

	free(self);
}

int disk_image__read_sector(struct disk_image *self, uint64_t sector, void *dst, uint32_t dst_len)
{
	uint64_t offset = sector << SECTOR_SHIFT;

	if (offset + dst_len > self->size)
		return -1;

	if (lseek(self->fd, offset, SEEK_SET) < 0)
		return -1;

	if (read_in_full(self->fd, dst, dst_len) < 0)
		return -1;

	return 0;
}

int disk_image__write_sector(struct disk_image *self, uint64_t sector, void *src, uint32_t src_len)
{
	uint64_t offset = sector << SECTOR_SHIFT;

	if (offset + src_len > self->size)
		return -1;

	if (lseek(self->fd, offset, SEEK_SET) < 0)
		return -1;

	if (write_in_full(self->fd, src, src_len) < 0)
		return -1;

	return 0;
}
