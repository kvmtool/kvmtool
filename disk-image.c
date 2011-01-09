#include "kvm/disk-image.h"

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

static const char QCOW_MAGIC[]	= { 'Q', 'F', 'I', 0xfb };

struct qcow_header {
	uint8_t			magic[5];
};

static bool disk_image__is_qcow(struct disk_image *self)
{
	struct qcow_header *header = self->mmap;

	return !memcmp(header->magic, QCOW_MAGIC, ARRAY_SIZE(QCOW_MAGIC));
}

struct disk_image *disk_image__open(const char *filename)
{
	struct disk_image *self;
	struct stat st;

	self		= malloc(sizeof *self);
	if (!self)
		return NULL;

	self->fd	= open(filename, O_RDONLY);
	if (self->fd < 0)
		goto failed_free;

	if (fstat(self->fd, &st) < 0)
		goto failed_close_fd;

	self->size	= st.st_size;

	self->mmap	= mmap(NULL, self->size, PROT_READ|PROT_WRITE, MAP_PRIVATE, self->fd, 0);
	if (self->mmap == MAP_FAILED)
		goto failed_close_fd;

	if (disk_image__is_qcow(self))
		die("QCOW disk image format is not supported.");

	return self;

failed_close_fd:
	close(self->fd);
failed_free:
	free(self);

	return NULL;
}

void disk_image__close(struct disk_image *self)
{
	if (munmap(self->mmap, self->size) < 0)
		warning("munmap() failed");

	if (close(self->fd) < 0)
		warning("close() failed");

	free(self);
}

int disk_image__read_sector(struct disk_image *self, uint64_t sector, void *dst, uint32_t dst_len)
{
	uint64_t offset = sector << SECTOR_SHIFT;

	if (offset + dst_len > self->size)
		return -1;

	memcpy(dst, self->mmap + offset, dst_len);

	return 0;
}

int disk_image__write_sector(struct disk_image *self, uint64_t sector, void *src, uint32_t src_len)
{
	uint64_t offset = sector << SECTOR_SHIFT;

	if (offset + src_len > self->size)
		return -1;

	memcpy(self->mmap + offset, src, src_len);

	return 0;
}
