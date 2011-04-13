#include "kvm/qcow.h"

#include "kvm/disk-image.h"
#include "kvm/read-write.h"
#include "kvm/util.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <linux/byteorder.h>
#include <linux/types.h>

static inline uint64_t sect_to_l1_offset(struct qcow *q, uint64_t offset)
{
	struct qcow1_header *header = q->header;

	return offset >> (header->l2_bits + header->cluster_bits);
}

static inline uint64_t sect_to_l2_offset(struct qcow *q, uint64_t offset)
{
	struct qcow1_header *header = q->header;

	return (offset >> (header->cluster_bits)) & ((1 << header->l2_bits)-1);
}

static inline uint64_t sect_to_cluster_offset(struct qcow *q, uint64_t offset)
{
	struct qcow1_header *header = q->header;

	return offset & ((1 << header->cluster_bits)-1);
}

static int qcow1_read_sector(struct disk_image *self, uint64_t sector, void *dst, uint32_t dst_len)
{
	struct qcow *q = self->priv;
	struct qcow1_header *header = q->header;
	uint64_t l2_table_offset;
	uint64_t l2_table_size;
	uint64_t clust_offset;
	uint64_t clust_start;
	uint64_t *l2_table;
	uint64_t l1_idx;
	uint64_t l2_idx;
	uint64_t offset;

	offset		= sector << SECTOR_SHIFT;
	if (offset >= header->size)
		goto out_error;

	l1_idx		= sect_to_l1_offset(self->priv, offset);

	if (l1_idx >= q->table.table_size)
		goto out_error;

	l2_table_offset	= be64_to_cpu(q->table.l1_table[l1_idx]);
	if (!l2_table_offset)
		goto zero_sector;

	l2_table_size	= 1 << header->l2_bits;

	l2_table	= calloc(l2_table_size, sizeof(uint64_t));
	if (!l2_table)
		goto out_error;

	if (pread_in_full(q->fd, l2_table, sizeof(uint64_t) * l2_table_size, l2_table_offset) < 0)
		goto out_error_free_l2;

	l2_idx		= sect_to_l2_offset(self->priv, offset);

	if (l2_idx >= l2_table_size)
		goto out_error_free_l2;

	clust_start	= be64_to_cpu(l2_table[l2_idx]);

	if (!clust_start)
		goto zero_sector;

	clust_offset	= sect_to_cluster_offset(self->priv, offset);

	if (pread_in_full(q->fd, dst, dst_len, clust_start + clust_offset) < 0)
		goto out_error_free_l2;

	free(l2_table);

	return 0;

zero_sector:
	memset(dst, 0, dst_len);

	return 0;

out_error_free_l2:
	free(l2_table);
out_error:
	return -1;
}

static int qcow1_write_sector(struct disk_image *self, uint64_t sector, void *src, uint32_t src_len)
{
	return -1;
}

static void qcow1_disk_close(struct disk_image *self)
{
	struct qcow *q;

	if (!self)
		return;

	q = self->priv;

	free(q->header);
	free(q);
}

struct disk_image_operations qcow1_disk_ops = {
	.read_sector		= qcow1_read_sector,
	.write_sector		= qcow1_write_sector,
	.close			= qcow1_disk_close
};

static int qcow_read_l1_table(struct qcow *q)
{
	struct qcow1_header *header = q->header;

	q->table.table_size	= header->size / ((1 << header->l2_bits) * (1 << header->cluster_bits));

	q->table.l1_table	= calloc(q->table.table_size, sizeof(uint64_t));
	if (!q->table.l1_table)
		return -1;

	if (pread_in_full(q->fd, q->table.l1_table, sizeof(uint64_t) * q->table.table_size, header->l1_table_offset) < 0)
		return -1;

	return 0;
}

static void *qcow1_read_header(int fd)
{
	struct qcow1_header *header;

	header = malloc(sizeof(struct qcow1_header));
	if (!header)
		return NULL;

	if (pread_in_full(fd, header, sizeof(struct qcow1_header), 0) < 0)
		return NULL;

	be32_to_cpus(&header->magic);
	be32_to_cpus(&header->version);
	be64_to_cpus(&header->backing_file_offset);
	be32_to_cpus(&header->backing_file_size);
	be32_to_cpus(&header->mtime);
	be64_to_cpus(&header->size);
	be32_to_cpus(&header->crypt_method);
	be64_to_cpus(&header->l1_table_offset);

	return header;
}

static struct disk_image *qcow1_probe(int fd)
{
	struct qcow *q;
	struct qcow1_header *h;
	struct disk_image *disk_image;

	q = calloc(1, sizeof(struct qcow));
	if (!q)
		goto error;

	q->fd = fd;

	h = q->header = qcow1_read_header(fd);
	if (!h)
		goto error;

	if (qcow_read_l1_table(q) < 0)
		goto error;

	disk_image = disk_image__new(fd, h->size, &qcow1_disk_ops);
	if (!disk_image)
		goto error;
	disk_image->priv = q;

	return disk_image;
error:
	if (!q)
		return NULL;

	free(q->header);
	free(q);

	return NULL;
}

static int qcow_check_image(int fd)
{
	struct qcow1_header header;

	if (pread_in_full(fd, &header, sizeof(struct qcow1_header), 0) < 0)
		return -1;

	be32_to_cpus(&header.magic);
	be32_to_cpus(&header.version);

	if (header.magic != QCOW_MAGIC)
		return -1;

	if (header.version != QCOW1_VERSION)
		return -1;

	return 0;
}

struct disk_image *qcow_probe(int fd)
{
	if (qcow_check_image(fd) < 0)
		return NULL;

	return qcow1_probe(fd);
}
