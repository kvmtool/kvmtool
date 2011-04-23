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
	struct qcow_header *header = q->header;

	return offset >> (header->l2_bits + header->cluster_bits);
}

static inline u64 get_l2_index(struct qcow *q, u64 offset)
{
	struct qcow_header *header = q->header;

	return (offset >> (header->cluster_bits)) & ((1 << header->l2_bits)-1);
}

static inline u64 get_cluster_offset(struct qcow *q, u64 offset)
{
	struct qcow_header *header = q->header;

	return offset & ((1 << header->cluster_bits)-1);
}

static ssize_t qcow1_read_cluster(struct qcow *q, u64 offset, void *dst, u32 dst_len)
{
	struct qcow_header *header = q->header;
	struct qcow_table *table  = &q->table;
	u64 *l2_table = NULL;
	u64 l2_table_offset;
	u64 l2_table_size;
	u64 cluster_size;
	u64 clust_offset;
	u64 clust_start;
	size_t length;
	u64 l1_idx;
	u64 l2_idx;

	cluster_size = 1 << header->cluster_bits;

	l1_idx = get_l1_index(q, offset);
	if (l1_idx >= table->table_size)
		goto out_error;

	clust_offset = get_cluster_offset(q, offset);
	if (clust_offset >= cluster_size)
		goto out_error;

	length = cluster_size - clust_offset;
	if (length > dst_len)
		length = dst_len;

	l2_table_offset = table->l1_table[l1_idx] & ~header->oflag_mask;
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

	clust_start = be64_to_cpu(l2_table[l2_idx]) & ~header->oflag_mask;
	if (!clust_start)
		goto zero_cluster;

	if (pread_in_full(q->fd, dst, length, clust_start + clust_offset) < 0)
		goto out_error;

out:
	free(l2_table);
	return length;

zero_cluster:
	memset(dst, 0, length);
	goto out;

out_error:
	length = -1;
	goto out;
}

static int qcow1_read_sector(struct disk_image *self, uint64_t sector,
		void *dst, uint32_t dst_len)
{
	struct qcow *q = self->priv;
	struct qcow_header *header = q->header;
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
		if (nr <= 0)
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
	struct qcow_header *header = q->header;
	struct qcow_table *table = &q->table;
	u64 i;

	table->table_size	= header->l1_size;

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

static void *qcow2_read_header(int fd)
{
	struct qcow2_header_disk f_header;
	struct qcow_header *header;

	header = malloc(sizeof(struct qcow_header));
	if (!header)
		return NULL;

	if (pread_in_full(fd, &f_header, sizeof(struct qcow2_header_disk), 0) < 0)
		return NULL;

	be32_to_cpus(&f_header.magic);
	be32_to_cpus(&f_header.version);
	be64_to_cpus(&f_header.backing_file_offset);
	be32_to_cpus(&f_header.backing_file_size);
	be32_to_cpus(&f_header.cluster_bits);
	be64_to_cpus(&f_header.size);
	be32_to_cpus(&f_header.crypt_method);
	be32_to_cpus(&f_header.l1_size);
	be64_to_cpus(&f_header.l1_table_offset);
	be64_to_cpus(&f_header.refcount_table_offset);
	be32_to_cpus(&f_header.refcount_table_clusters);
	be32_to_cpus(&f_header.nb_snapshots);
	be64_to_cpus(&f_header.snapshots_offset);

	*header		= (struct qcow_header) {
		.size			= f_header.size,
		.l1_table_offset	= f_header.l1_table_offset,
		.l1_size		= f_header.l1_size,
		.cluster_bits		= f_header.cluster_bits,
		.l2_bits		= f_header.cluster_bits - 3,
		.oflag_mask		= QCOW2_OFLAG_MASK,
	};

	return header;
}

static struct disk_image *qcow2_probe(int fd)
{
	struct qcow *q;
	struct qcow_header *h;
	struct disk_image *disk_image;

	q = calloc(1, sizeof(struct qcow));
	if (!q)
		goto error;

	q->fd = fd;

	h = q->header = qcow2_read_header(fd);
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

static bool qcow2_check_image(int fd)
{
	struct qcow2_header_disk f_header;

	if (pread_in_full(fd, &f_header, sizeof(struct qcow2_header_disk), 0) < 0)
		return false;

	be32_to_cpus(&f_header.magic);
	be32_to_cpus(&f_header.version);

	if (f_header.magic != QCOW_MAGIC)
		return false;

	if (f_header.version != QCOW2_VERSION)
		return false;

	return true;
}

static void *qcow1_read_header(int fd)
{
	struct qcow1_header_disk f_header;
	struct qcow_header *header;

	header = malloc(sizeof(struct qcow_header));
	if (!header)
		return NULL;

	if (pread_in_full(fd, &f_header, sizeof(struct qcow1_header_disk), 0) < 0) {
		free(header);
		return NULL;
	}

	be32_to_cpus(&f_header.magic);
	be32_to_cpus(&f_header.version);
	be64_to_cpus(&f_header.backing_file_offset);
	be32_to_cpus(&f_header.backing_file_size);
	be32_to_cpus(&f_header.mtime);
	be64_to_cpus(&f_header.size);
	be32_to_cpus(&f_header.crypt_method);
	be64_to_cpus(&f_header.l1_table_offset);

	*header		= (struct qcow_header) {
		.size			= f_header.size,
		.l1_table_offset	= f_header.l1_table_offset,
		.l1_size		= f_header.size / ((1 << f_header.l2_bits) * (1 << f_header.cluster_bits)),
		.cluster_bits		= f_header.cluster_bits,
		.l2_bits		= f_header.l2_bits,
		.oflag_mask		= QCOW1_OFLAG_MASK,
	};

	return header;
}

static struct disk_image *qcow1_probe(int fd)
{
	struct qcow *q;
	struct qcow_header *h;
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

static bool qcow1_check_image(int fd)
{
	struct qcow1_header_disk f_header;

	if (pread_in_full(fd, &f_header, sizeof(struct qcow1_header_disk), 0) < 0)
		return false;

	be32_to_cpus(&f_header.magic);
	be32_to_cpus(&f_header.version);

	if (f_header.magic != QCOW_MAGIC)
		return false;

	if (f_header.version != QCOW1_VERSION)
		return false;

	return true;
}

struct disk_image *qcow_probe(int fd)
{
	if (qcow1_check_image(fd))
		return qcow1_probe(fd);

	if (qcow2_check_image(fd))
		return qcow2_probe(fd);

	return NULL;
}
