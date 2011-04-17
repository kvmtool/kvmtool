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

static inline u64 get_l1_index(struct qcow *q, u64 offset)
{
	struct qcow1_header *header = q->header;

	return offset >> (header->l2_bits + header->cluster_bits);
}

static inline u64 get_l2_index(struct qcow *q, u64 offset)
{
	struct qcow1_header *header = q->header;

	return (offset >> (header->cluster_bits)) & ((1 << header->l2_bits)-1);
}

static inline u64 get_cluster_offset(struct qcow *q, u64 offset)
{
	struct qcow1_header *header = q->header;

	return offset & ((1 << header->cluster_bits)-1);
}

static u32 qcow1_read_cluster(struct qcow *q, u64 offset, void *dst,
		u32 dst_len)
{
	struct qcow1_header *header = q->header;
	struct qcow_table   *table  = &q->table;
	u64 l2_table_offset;
	u64 l2_table_size;
	u64 cluster_size;
	u64 clust_offset;
	u64 clust_start;
	u64 *l2_table = NULL;
	u64 l1_idx;
	u64 l2_idx;
	u32 length;

	length = cluster_size = 1 << header->cluster_bits;

	l1_idx = get_l1_index(q, offset);
	if (l1_idx >= table->table_size)
		goto out_error;

	clust_offset = get_cluster_offset(q, offset);
	if (clust_offset >= cluster_size)
		goto out_error;

	length = cluster_size - clust_offset;
	if (length > dst_len)
		length = dst_len;

	l2_table_offset = table->l1_table[l1_idx];
	if (!l2_table_offset)
		goto zero_cluster;

	l2_table_size = 1 << header->l2_bits;
	l2_table = calloc(l2_table_size, sizeof(u64));
	if (!l2_table)
		goto out_error;

	if (pread_in_full(q->fd, l2_table, l2_table_size * sizeof(u64),
				l2_table_offset) < 0)
		goto out_error;

	l2_idx = get_l2_index(q, offset);
	if (l2_idx >= l2_table_size)
		goto out_error;

	clust_start = be64_to_cpu(l2_table[l2_idx]);
	free(l2_table);
	if (!clust_start)
		goto zero_cluster;

	if (pread_in_full(q->fd, dst, length, clust_start + clust_offset) < 0)
		goto out_error;

	return length;
zero_cluster:
	memset(dst, 0, length);
	return length;

out_error:
	free(l2_table);
	return -1;
}

static int qcow1_read_sector(struct disk_image *self, uint64_t sector,
		void *dst, uint32_t dst_len)
{
	struct qcow *q = self->priv;
	struct qcow1_header *header = q->header;
	char *buf = dst;
	u64 offset;
	u32 nr_read;
	u32 nr;

	nr_read = 0;
	while (nr_read < dst_len) {
		offset = sector << SECTOR_SHIFT;
		if (offset >= header->size)
			goto out_error;

		nr = qcow1_read_cluster(q, offset, buf, dst_len - nr_read);
		if (!nr)
			goto out_error;

		nr_read		+= nr;
		buf		+= nr;
		sector		+= (nr >> SECTOR_SHIFT);
	}
	return 0;
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

	free(q->table.l1_table);
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
	struct qcow_table *table = &q->table;
	u64 i;

	table->table_size = header->size / ((1 << header->l2_bits) *
			(1 << header->cluster_bits));

	table->l1_table	= calloc(table->table_size, sizeof(u64));
	if (!table->l1_table)
		return -1;

	if (pread_in_full(q->fd, table->l1_table, sizeof(u64) *
				table->table_size, header->l1_table_offset) < 0)
		return -1;

	for (i = 0; i < table->table_size; i++)
		be64_to_cpus(&table->l1_table[i]);

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

	free(q->table.l1_table);
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
