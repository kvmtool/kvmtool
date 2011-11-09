#include "kvm/qcow.h"

#include "kvm/disk-image.h"
#include "kvm/read-write.h"
#include "kvm/mutex.h"
#include "kvm/util.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef CONFIG_HAS_ZLIB
#include <zlib.h>
#endif

#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/types.h>

static int l2_table_insert(struct rb_root *root, struct qcow_l2_table *new)
{
	struct rb_node **link = &(root->rb_node), *parent = NULL;
	u64 offset = new->offset;

	/* search the tree */
	while (*link) {
		struct qcow_l2_table *t;

		t = rb_entry(*link, struct qcow_l2_table, node);
		if (!t)
			goto error;

		parent = *link;

		if (t->offset > offset)
			link = &(*link)->rb_left;
		else if (t->offset < offset)
			link = &(*link)->rb_right;
		else
			goto out;
	}

	/* add new node */
	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, root);
out:
	return 0;
error:
	return -1;
}

static struct qcow_l2_table *l2_table_lookup(struct rb_root *root, u64 offset)
{
	struct rb_node *link = root->rb_node;

	while (link) {
		struct qcow_l2_table *t;

		t = rb_entry(link, struct qcow_l2_table, node);
		if (!t)
			goto out;

		if (t->offset > offset)
			link = link->rb_left;
		else if (t->offset < offset)
			link = link->rb_right;
		else
			return t;
	}
out:
	return NULL;
}

static void l1_table_free_cache(struct qcow_l1_table *l1t)
{
	struct rb_root *r = &l1t->root;
	struct list_head *pos, *n;
	struct qcow_l2_table *t;

	list_for_each_safe(pos, n, &l1t->lru_list) {
		/* Remove cache table from the list and RB tree */
		list_del(pos);
		t = list_entry(pos, struct qcow_l2_table, list);
		rb_erase(&t->node, r);

		/* Free the cached node */
		free(t);
	}
}

static int qcow_l2_cache_write(struct qcow *q, struct qcow_l2_table *c)
{
	struct qcow_header *header = q->header;
	u64 size;

	if (!c->dirty)
		return 0;

	size = 1 << header->l2_bits;

	if (pwrite_in_full(q->fd, c->table, size * sizeof(u64), c->offset) < 0)
		return -1;

	c->dirty = 0;

	return 0;
}

static int cache_table(struct qcow *q, struct qcow_l2_table *c)
{
	struct qcow_l1_table *l1t = &q->table;
	struct rb_root *r = &l1t->root;
	struct qcow_l2_table *lru;

	if (l1t->nr_cached == MAX_CACHE_NODES) {
		/*
		 * The node at the head of the list is least recently used
		 * node. Remove it from the list and replaced with a new node.
		 */
		lru = list_first_entry(&l1t->lru_list, struct qcow_l2_table, list);

		if (qcow_l2_cache_write(q, lru) < 0)
			goto error;

		/* Remove the node from the cache */
		rb_erase(&lru->node, r);
		list_del_init(&lru->list);
		l1t->nr_cached--;

		/* Free the LRUed node */
		free(lru);
	}

	/* Add new node in RB Tree: Helps in searching faster */
	if (l2_table_insert(r, c) < 0)
		goto error;

	/* Add in LRU replacement list */
	list_add_tail(&c->list, &l1t->lru_list);
	l1t->nr_cached++;

	return 0;
error:
	return -1;
}

static struct qcow_l2_table *l2_table_search(struct qcow *q, u64 offset)
{
	struct qcow_l1_table *l1t = &q->table;
	struct qcow_l2_table *l2t;

	l2t = l2_table_lookup(&l1t->root, offset);
	if (!l2t)
		return NULL;

	/* Update the LRU state, by moving the searched node to list tail */
	list_move_tail(&l2t->list, &l1t->lru_list);

	return l2t;
}

/* Allocates a new node for caching L2 table */
static struct qcow_l2_table *new_cache_table(struct qcow *q, u64 offset)
{
	struct qcow_header *header = q->header;
	struct qcow_l2_table *c;
	u64 l2t_sz;
	u64 size;

	l2t_sz = 1 << header->l2_bits;
	size   = sizeof(*c) + l2t_sz * sizeof(u64);
	c      = calloc(1, size);
	if (!c)
		goto out;

	c->offset = offset;
	RB_CLEAR_NODE(&c->node);
	INIT_LIST_HEAD(&c->list);
out:
	return c;
}

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

static struct qcow_l2_table *qcow_read_l2_table(struct qcow *q, u64 offset)
{
	struct qcow_header *header = q->header;
	struct qcow_l2_table *l2t;
	u64 size;

	size = 1 << header->l2_bits;

	/* search an entry for offset in cache */
	l2t = l2_table_search(q, offset);
	if (l2t)
		return l2t;

	/* allocate new node for caching l2 table */
	l2t = new_cache_table(q, offset);
	if (!l2t)
		goto error;

	/* table not cached: read from the disk */
	if (pread_in_full(q->fd, l2t->table, size * sizeof(u64), offset) < 0)
		goto error;

	/* cache the table */
	if (cache_table(q, l2t) < 0)
		goto error;

	return l2t;
error:
	free(l2t);
	return NULL;
}

static int qcow_decompress_buffer(u8 *out_buf, int out_buf_size,
	const u8 *buf, int buf_size)
{
#ifdef CONFIG_HAS_ZLIB
	z_stream strm1, *strm = &strm1;
	int ret, out_len;

	memset(strm, 0, sizeof(*strm));

	strm->next_in = (u8 *)buf;
	strm->avail_in = buf_size;
	strm->next_out = out_buf;
	strm->avail_out = out_buf_size;

	ret = inflateInit2(strm, -12);
	if (ret != Z_OK)
		return -1;

	ret = inflate(strm, Z_FINISH);
	out_len = strm->next_out - out_buf;
	if ((ret != Z_STREAM_END && ret != Z_BUF_ERROR) ||
		out_len != out_buf_size) {
		inflateEnd(strm);
		return -1;
	}

	inflateEnd(strm);
	return 0;
#else
	return -1;
#endif
}

static ssize_t qcow1_read_cluster(struct qcow *q, u64 offset,
	void *dst, u32 dst_len)
{
	struct qcow_header *header = q->header;
	struct qcow_l1_table *l1t = &q->table;
	struct qcow_l2_table *l2t;
	u64 clust_offset;
	u64 clust_start;
	u64 l2t_offset;
	size_t length;
	u64 l2t_size;
	u64 l1_idx;
	u64 l2_idx;
	int coffset;
	int csize;

	l1_idx = get_l1_index(q, offset);
	if (l1_idx >= l1t->table_size)
		return -1;

	clust_offset = get_cluster_offset(q, offset);
	if (clust_offset >= q->cluster_size)
		return -1;

	length = q->cluster_size - clust_offset;
	if (length > dst_len)
		length = dst_len;

	mutex_lock(&q->mutex);

	l2t_offset = be64_to_cpu(l1t->l1_table[l1_idx]);
	if (!l2t_offset)
		goto zero_cluster;

	l2t_size = 1 << header->l2_bits;

	/* read and cache level 2 table */
	l2t = qcow_read_l2_table(q, l2t_offset);
	if (!l2t)
		goto out_error;

	l2_idx = get_l2_index(q, offset);
	if (l2_idx >= l2t_size)
		goto out_error;

	clust_start = be64_to_cpu(l2t->table[l2_idx]);
	if (clust_start & QCOW1_OFLAG_COMPRESSED) {
		coffset = clust_start & q->cluster_offset_mask;
		csize = clust_start >> (63 - q->header->cluster_bits);
		csize &= (q->cluster_size - 1);

		if (pread_in_full(q->fd, q->cluster_data, csize,
				  coffset) < 0) {
			goto out_error;
		}

		if (qcow_decompress_buffer(q->cluster_cache, q->cluster_size,
					q->cluster_data, csize) < 0) {
			goto out_error;
		}

		memcpy(dst, q->cluster_cache + clust_offset, length);
		mutex_unlock(&q->mutex);
	} else{
		if (!clust_start)
			goto zero_cluster;

		mutex_unlock(&q->mutex);

		if (pread_in_full(q->fd, dst, length,
				  clust_start + clust_offset) < 0)
			return -1;
	}

	return length;

zero_cluster:
	mutex_unlock(&q->mutex);
	memset(dst, 0, length);
	return length;

out_error:
	mutex_unlock(&q->mutex);
	length = -1;
	return -1;
}

static ssize_t qcow2_read_cluster(struct qcow *q, u64 offset,
	void *dst, u32 dst_len)
{
	struct qcow_header *header = q->header;
	struct qcow_l1_table *l1t = &q->table;
	struct qcow_l2_table *l2t;
	u64 clust_offset;
	u64 clust_start;
	u64 l2t_offset;
	size_t length;
	u64 l2t_size;
	u64 l1_idx;
	u64 l2_idx;
	int coffset;
	int sector_offset;
	int nb_csectors;
	int csize;

	l1_idx = get_l1_index(q, offset);
	if (l1_idx >= l1t->table_size)
		return -1;

	clust_offset = get_cluster_offset(q, offset);
	if (clust_offset >= q->cluster_size)
		return -1;

	length = q->cluster_size - clust_offset;
	if (length > dst_len)
		length = dst_len;

	mutex_lock(&q->mutex);

	l2t_offset = be64_to_cpu(l1t->l1_table[l1_idx]);

	l2t_offset &= ~QCOW2_OFLAG_COPIED;
	if (!l2t_offset)
		goto zero_cluster;

	l2t_size = 1 << header->l2_bits;

	/* read and cache level 2 table */
	l2t = qcow_read_l2_table(q, l2t_offset);
	if (!l2t)
		goto out_error;

	l2_idx = get_l2_index(q, offset);
	if (l2_idx >= l2t_size)
		goto out_error;

	clust_start = be64_to_cpu(l2t->table[l2_idx]);
	if (clust_start & QCOW2_OFLAG_COMPRESSED) {
		coffset = clust_start & q->cluster_offset_mask;
		nb_csectors = ((clust_start >> q->csize_shift)
			& q->csize_mask) + 1;
		sector_offset = coffset & (SECTOR_SIZE - 1);
		csize = nb_csectors * SECTOR_SIZE - sector_offset;

		if (pread_in_full(q->fd, q->cluster_data,
				  nb_csectors * SECTOR_SIZE,
				  coffset & ~(SECTOR_SIZE - 1)) < 0) {
			goto out_error;
		}

		if (qcow_decompress_buffer(q->cluster_cache, q->cluster_size,
					q->cluster_data + sector_offset,
					csize) < 0) {
			goto out_error;
		}

		memcpy(dst, q->cluster_cache + clust_offset, length);
		mutex_unlock(&q->mutex);
	} else{
		clust_start &= QCOW2_OFFSET_MASK;
		if (!clust_start)
			goto zero_cluster;

		mutex_unlock(&q->mutex);

		if (pread_in_full(q->fd, dst, length,
				  clust_start + clust_offset) < 0)
			return -1;
	}

	return length;

zero_cluster:
	mutex_unlock(&q->mutex);
	memset(dst, 0, length);
	return length;

out_error:
	mutex_unlock(&q->mutex);
	length = -1;
	return -1;
}

static ssize_t qcow_read_sector_single(struct disk_image *disk, u64 sector,
	void *dst, u32 dst_len)
{
	struct qcow *q = disk->priv;
	struct qcow_header *header = q->header;
	u32 nr_read;
	u64 offset;
	char *buf;
	u32 nr;

	buf		= dst;
	nr_read		= 0;

	while (nr_read < dst_len) {
		offset		= sector << SECTOR_SHIFT;
		if (offset >= header->size)
			return -1;

		if (q->version == QCOW1_VERSION)
			nr = qcow1_read_cluster(q, offset, buf,
				dst_len - nr_read);
		else
			nr = qcow2_read_cluster(q, offset, buf,
				dst_len - nr_read);

		if (nr <= 0)
			return -1;

		nr_read		+= nr;
		buf		+= nr;
		sector		+= (nr >> SECTOR_SHIFT);
	}

	return dst_len;
}

static ssize_t qcow_read_sector(struct disk_image *disk, u64 sector,
				const struct iovec *iov, int iovcount, void *param)
{
	ssize_t nr, total = 0;

	while (iovcount--) {
		nr = qcow_read_sector_single(disk, sector, iov->iov_base, iov->iov_len);
		if (nr != (ssize_t)iov->iov_len) {
			pr_info("qcow_read_sector error: nr=%ld iov_len=%ld\n", (long)nr, (long)iov->iov_len);
			return -1;
		}

		sector	+= iov->iov_len >> SECTOR_SHIFT;
		iov++;
		total	+= nr;
	}

	return total;
}

static inline u64 file_size(int fd)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return 0;

	return st.st_size;
}

static inline int qcow_pwrite_sync(int fd, void *buf, size_t count, off_t offset)
{
	if (pwrite_in_full(fd, buf, count, offset) < 0)
		return -1;

	return fdatasync(fd);
}

/* Writes a level 2 table at the end of the file. */
static u64 qcow_write_l2_table(struct qcow *q, u64 *table)
{
	struct qcow_header *header = q->header;
	u64 clust_sz;
	u64 f_sz;
	u64 off;
	u64 sz;

	f_sz		= file_size(q->fd);
	if (!f_sz)
		return 0;

	sz		= 1 << header->l2_bits;
	clust_sz	= 1 << header->cluster_bits;
	off		= ALIGN(f_sz, clust_sz);

	if (pwrite_in_full(q->fd, table, sz * sizeof(u64), off) < 0)
		return 0;

	return off;
}

static void refcount_table_free_cache(struct qcow_refcount_table *rft)
{
	struct rb_root *r = &rft->root;
	struct list_head *pos, *n;
	struct qcow_refcount_block *t;

	list_for_each_safe(pos, n, &rft->lru_list) {
		list_del(pos);
		t = list_entry(pos, struct qcow_refcount_block, list);
		rb_erase(&t->node, r);

		free(t);
	}
}

static int refcount_block_insert(struct rb_root *root, struct qcow_refcount_block *new)
{
	struct rb_node **link = &(root->rb_node), *parent = NULL;
	u64 offset = new->offset;

	/* search the tree */
	while (*link) {
		struct qcow_refcount_block *t;

		t = rb_entry(*link, struct qcow_refcount_block, node);
		if (!t)
			goto error;

		parent = *link;

		if (t->offset > offset)
			link = &(*link)->rb_left;
		else if (t->offset < offset)
			link = &(*link)->rb_right;
		else
			goto out;
	}

	/* add new node */
	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, root);
out:
	return 0;
error:
	return -1;
}

static int write_refcount_block(struct qcow *q, struct qcow_refcount_block *rfb)
{
	if (!rfb->dirty)
		return 0;

	if (pwrite_in_full(q->fd, rfb->entries, rfb->size * sizeof(u16), rfb->offset) < 0)
		return -1;

	rfb->dirty = 0;

	return 0;
}

static int cache_refcount_block(struct qcow *q, struct qcow_refcount_block *c)
{
	struct qcow_refcount_table *rft = &q->refcount_table;
	struct rb_root *r = &rft->root;
	struct qcow_refcount_block *lru;

	if (rft->nr_cached == MAX_CACHE_NODES) {
		lru = list_first_entry(&rft->lru_list, struct qcow_refcount_block, list);

		if (write_refcount_block(q, lru) < 0)
			goto error;

		rb_erase(&lru->node, r);
		list_del_init(&lru->list);
		rft->nr_cached--;

		free(lru);
	}

	if (refcount_block_insert(r, c) < 0)
		goto error;

	list_add_tail(&c->list, &rft->lru_list);
	rft->nr_cached++;

	return 0;
error:
	return -1;
}

static struct qcow_refcount_block *new_refcount_block(struct qcow *q, u64 rfb_offset)
{
	struct qcow_refcount_block *rfb;

	rfb = malloc(sizeof *rfb + q->cluster_size);
	if (!rfb)
		return NULL;

	rfb->offset = rfb_offset;
	rfb->size = q->cluster_size / sizeof(u16);
	RB_CLEAR_NODE(&rfb->node);
	INIT_LIST_HEAD(&rfb->list);

	return rfb;
}

static struct qcow_refcount_block *refcount_block_lookup(struct rb_root *root, u64 offset)
{
	struct rb_node *link = root->rb_node;

	while (link) {
		struct qcow_refcount_block *t;

		t = rb_entry(link, struct qcow_refcount_block, node);
		if (!t)
			goto out;

		if (t->offset > offset)
			link = link->rb_left;
		else if (t->offset < offset)
			link = link->rb_right;
		else
			return t;
	}
out:
	return NULL;
}

static struct qcow_refcount_block *refcount_block_search(struct qcow *q, u64 offset)
{
	struct qcow_refcount_table *rft = &q->refcount_table;
	struct qcow_refcount_block *rfb;

	rfb = refcount_block_lookup(&rft->root, offset);
	if (!rfb)
		return NULL;

	/* Update the LRU state, by moving the searched node to list tail */
	list_move_tail(&rfb->list, &rft->lru_list);

	return rfb;
}

static struct qcow_refcount_block *qcow_read_refcount_block(struct qcow *q, u64 clust_idx)
{
	struct qcow_header *header = q->header;
	struct qcow_refcount_table *rft = &q->refcount_table;
	struct qcow_refcount_block *rfb;
	u64 rfb_offset;
	u64 rft_idx;

	rft_idx = clust_idx >> (header->cluster_bits - QCOW_REFCOUNT_BLOCK_SHIFT);
	if (rft_idx >= rft->rf_size)
		return NULL;

	rfb_offset = be64_to_cpu(rft->rf_table[rft_idx]);

	rfb = refcount_block_search(q, rfb_offset);
	if (rfb)
		return rfb;

	rfb = new_refcount_block(q, rfb_offset);
	if (!rfb)
		return NULL;

	if (pread_in_full(q->fd, rfb->entries, rfb->size * sizeof(u16), rfb_offset) < 0)
		goto error_free_rfb;

	if (cache_refcount_block(q, rfb) < 0)
		goto error_free_rfb;

	return rfb;

error_free_rfb:
	free(rfb);

	return NULL;
}

/*
 * QCOW file might grow during a write operation. Not only data but metadata is
 * also written at the end of the file. Therefore it is necessary to ensure
 * every write is committed to disk. Hence we use uses qcow_pwrite_sync() to
 * synchronize the in-core state of QCOW image to disk.
 *
 * We also try to restore the image to a consistent state if the metdata
 * operation fails. The two metadat operations are: level 1 and level 2 table
 * update. If either of them fails the image is truncated to a consistent state.
 */
static ssize_t qcow_write_cluster(struct qcow *q, u64 offset, void *buf, u32 src_len)
{
	struct qcow_header *header = q->header;
	struct qcow_l1_table *l1t = &q->table;
	struct qcow_l2_table *l2t;
	u64 clust_start;
	u64 clust_flags;
	u64 l2t_offset;
	u64 clust_off;
	u64 l2t_size;
	u64 clust_sz;
	u64 l1t_idx;
	u64 l2t_idx;
	u64 f_sz;
	u64 len;

	l2t		= NULL;
	l2t_size	= 1 << header->l2_bits;
	clust_sz	= 1 << header->cluster_bits;

	l1t_idx = get_l1_index(q, offset);
	if (l1t_idx >= l1t->table_size)
		return -1;

	l2t_idx = get_l2_index(q, offset);
	if (l2t_idx >= l2t_size)
		return -1;

	clust_off = get_cluster_offset(q, offset);
	if (clust_off >= clust_sz)
		return -1;

	len = clust_sz - clust_off;
	if (len > src_len)
		len = src_len;

	mutex_lock(&q->mutex);

	l2t_offset = be64_to_cpu(l1t->l1_table[l1t_idx]);
	if (l2t_offset & QCOW2_OFLAG_COMPRESSED) {
		pr_warning("compressed clusters are not supported");
		goto error;
	}
	if (!(l2t_offset & QCOW2_OFLAG_COPIED)) {
		pr_warning("L2 copy-on-write clusters are not supported");
		goto error;
	}

	l2t_offset &= QCOW2_OFFSET_MASK;
	if (l2t_offset) {
		/* read and cache l2 table */
		l2t = qcow_read_l2_table(q, l2t_offset);
		if (!l2t)
			goto error;
	} else {
		l2t = new_cache_table(q, l2t_offset);
		if (!l2t)
			goto error;

		/* Capture the state of the consistent QCOW image */
		f_sz = file_size(q->fd);
		if (!f_sz)
			goto free_cache;

		/* Write the l2 table of 0's at the end of the file */
		l2t_offset = qcow_write_l2_table(q, l2t->table);
		if (!l2t_offset)
			goto free_cache;

		if (cache_table(q, l2t) < 0) {
			if (ftruncate(q->fd, f_sz) < 0)
				goto free_cache;

			goto free_cache;
		}

		/* Update the in-core entry */
		l1t->l1_table[l1t_idx] = cpu_to_be64(l2t_offset);
	}

	/* Capture the state of the consistent QCOW image */
	f_sz		= file_size(q->fd);
	if (!f_sz)
		goto error;

	clust_start = be64_to_cpu(l2t->table[l2t_idx]);

	clust_flags = clust_start & QCOW2_OFLAGS_MASK;
	if (clust_flags & QCOW2_OFLAG_COMPRESSED) {
		pr_warning("compressed clusters are not supported");
		goto error;
	}

	clust_start &= QCOW2_OFFSET_MASK;
	if (!clust_start) {
		clust_start		= ALIGN(f_sz, clust_sz);
		l2t->table[l2t_idx]	= cpu_to_be64(clust_start | QCOW2_OFLAG_COPIED);
		l2t->dirty		= 1;
	}

	if (!(clust_flags & QCOW2_OFLAG_COPIED)) {
		struct qcow_refcount_block *rfb = NULL;
		u16 clust_refcount;
		u64 clust_idx;
		u64 rfb_idx;

		clust_idx = (clust_start & QCOW2_OFFSET_MASK)
			>> (header->cluster_bits);

		rfb = qcow_read_refcount_block(q, clust_idx);
		if (!rfb) {
			pr_warning("L1: error while reading refcount table");
			goto error;
		}

		rfb_idx = clust_idx & (((1ULL << (header->cluster_bits - QCOW_REFCOUNT_BLOCK_SHIFT)) - 1));
		if (rfb_idx >= rfb->size) {
			pr_warning("L1: refcount block index out of bounds");
			goto error;
		}

		clust_refcount = be16_to_cpu(rfb->entries[rfb_idx]);
		if (!clust_refcount) {
			clust_refcount = 1;
			rfb->entries[rfb_idx] = cpu_to_be16(clust_refcount);
			rfb->dirty = 1;
		}

		if (clust_refcount > 1) {
			pr_warning("L1 copy-on-write clusters are not supported");
			goto error;
		}
	}

	mutex_unlock(&q->mutex);

	/* Write actual data */
	if (pwrite_in_full(q->fd, buf, len, clust_start + clust_off) < 0)
		return -1;

	return len;

free_cache:
	free(l2t);
error:
	mutex_unlock(&q->mutex);
	return -1;
}

static ssize_t qcow_write_sector_single(struct disk_image *disk, u64 sector, void *src, u32 src_len)
{
	struct qcow *q = disk->priv;
	struct qcow_header *header = q->header;
	u32 nr_written;
	char *buf;
	u64 offset;
	ssize_t nr;

	buf		= src;
	nr_written	= 0;
	offset		= sector << SECTOR_SHIFT;

	while (nr_written < src_len) {
		if (offset >= header->size)
			return -1;

		nr = qcow_write_cluster(q, offset, buf, src_len - nr_written);
		if (nr < 0)
			return -1;

		nr_written	+= nr;
		buf		+= nr;
		offset		+= nr;
	}

	return nr_written;
}

static ssize_t qcow_write_sector(struct disk_image *disk, u64 sector,
				const struct iovec *iov, int iovcount, void *param)
{
	ssize_t nr, total = 0;

	while (iovcount--) {
		nr = qcow_write_sector_single(disk, sector, iov->iov_base, iov->iov_len);
		if (nr != (ssize_t)iov->iov_len) {
			pr_info("qcow_write_sector error: nr=%ld iov_len=%ld\n", (long)nr, (long)iov->iov_len);
			return -1;
		}

		sector	+= iov->iov_len >> SECTOR_SHIFT;
		iov++;
		total	+= nr;
	}

	return total;
}

static int qcow_disk_flush(struct disk_image *disk)
{
	struct qcow *q = disk->priv;
	struct qcow_refcount_table *rft;
	struct qcow_header *header;
	struct list_head *pos, *n;
	struct qcow_l1_table *l1t;

	header = q->header;
	l1t = &q->table;
	rft = &q->refcount_table;

	mutex_lock(&q->mutex);

	list_for_each_safe(pos, n, &rft->lru_list) {
		struct qcow_refcount_block *c = list_entry(pos, struct qcow_refcount_block, list);

		if (write_refcount_block(q, c) < 0)
			goto error_unlock;
	}

	if (fdatasync(disk->fd) < 0)
		goto error_unlock;

	list_for_each_safe(pos, n, &l1t->lru_list) {
		struct qcow_l2_table *c = list_entry(pos, struct qcow_l2_table, list);

		if (qcow_l2_cache_write(q, c) < 0)
			goto error_unlock;
	}

	if (fdatasync(disk->fd) < 0)
		goto error_unlock;

	if (pwrite_in_full(disk->fd, l1t->l1_table, l1t->table_size * sizeof(u64), header->l1_table_offset) < 0)
		goto error_unlock;

	mutex_unlock(&q->mutex);

	return fsync(disk->fd);

error_unlock:
	mutex_unlock(&q->mutex);
	return -1;
}

static int qcow_disk_close(struct disk_image *disk)
{
	struct qcow *q;

	if (!disk)
		return 0;

	q = disk->priv;

	refcount_table_free_cache(&q->refcount_table);
	l1_table_free_cache(&q->table);
	free(q->cluster_data);
	free(q->cluster_cache);
	free(q->refcount_table.rf_table);
	free(q->table.l1_table);
	free(q->header);
	free(q);

	return 0;
}

static struct disk_image_operations qcow_disk_readonly_ops = {
	.read_sector		= qcow_read_sector,
	.close			= qcow_disk_close,
};

static struct disk_image_operations qcow_disk_ops = {
	.read_sector		= qcow_read_sector,
	.write_sector		= qcow_write_sector,
	.flush			= qcow_disk_flush,
	.close			= qcow_disk_close,
};

static int qcow_read_refcount_table(struct qcow *q)
{
	struct qcow_header *header = q->header;
	struct qcow_refcount_table *rft = &q->refcount_table;

	rft->rf_size = (header->refcount_table_size * q->cluster_size)
		/ sizeof(u64);

	rft->rf_table = calloc(rft->rf_size, sizeof(u64));
	if (!rft->rf_table)
		return -1;

	rft->root = RB_ROOT;
	INIT_LIST_HEAD(&rft->lru_list);

	return pread_in_full(q->fd, rft->rf_table, sizeof(u64) * rft->rf_size, header->refcount_table_offset);
}

static int qcow_read_l1_table(struct qcow *q)
{
	struct qcow_header *header = q->header;
	struct qcow_l1_table *table = &q->table;

	table->table_size	= header->l1_size;

	table->l1_table	= calloc(table->table_size, sizeof(u64));
	if (!table->l1_table)
		return -1;

	return pread_in_full(q->fd, table->l1_table, sizeof(u64) * table->table_size, header->l1_table_offset);
}

static void *qcow2_read_header(int fd)
{
	struct qcow2_header_disk f_header;
	struct qcow_header *header;

	header = malloc(sizeof(struct qcow_header));
	if (!header)
		return NULL;

	if (pread_in_full(fd, &f_header, sizeof(struct qcow2_header_disk), 0) < 0) {
		free(header);
		return NULL;
	}

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
		.refcount_table_offset	= f_header.refcount_table_offset,
		.refcount_table_size	= f_header.refcount_table_clusters,
	};

	return header;
}

static struct disk_image *qcow2_probe(int fd, bool readonly)
{
	struct disk_image *disk_image;
	struct qcow_l1_table *l1t;
	struct qcow_header *h;
	struct qcow *q;

	q = calloc(1, sizeof(struct qcow));
	if (!q)
		return NULL;

	mutex_init(&q->mutex);
	q->fd = fd;

	l1t = &q->table;

	l1t->root = RB_ROOT;
	INIT_LIST_HEAD(&l1t->lru_list);

	h = q->header = qcow2_read_header(fd);
	if (!h)
		goto free_qcow;

	q->version = QCOW2_VERSION;
	q->csize_shift = (62 - (q->header->cluster_bits - 8));
	q->csize_mask = (1 << (q->header->cluster_bits - 8)) - 1;
	q->cluster_offset_mask = (1LL << q->csize_shift) - 1;
	q->cluster_size = 1 << q->header->cluster_bits;

	q->cluster_data = malloc(q->cluster_size);
	if (!q->cluster_data) {
		pr_warning("cluster data malloc error!");
		goto free_header;
	}

	q->cluster_cache = malloc(q->cluster_size);
	if (!q->cluster_cache) {
		pr_warning("cluster cache malloc error!");
		goto free_cluster_data;
	}

	if (qcow_read_l1_table(q) < 0)
		goto free_cluster_cache;

	if (qcow_read_refcount_table(q) < 0)
		goto free_l1_table;

	/*
	 * Do not use mmap use read/write instead
	 */
	if (readonly)
		disk_image = disk_image__new(fd, h->size, &qcow_disk_readonly_ops, DISK_IMAGE_REGULAR);
	else
		disk_image = disk_image__new(fd, h->size, &qcow_disk_ops, DISK_IMAGE_REGULAR);

	if (!disk_image)
		goto free_refcount_table;

	disk_image->async = 0;
	disk_image->priv = q;

	return disk_image;

free_refcount_table:
	if (q->refcount_table.rf_table)
		free(q->refcount_table.rf_table);
free_l1_table:
	if (q->table.l1_table)
		free(q->table.l1_table);
free_cluster_cache:
	if (q->cluster_cache)
		free(q->cluster_cache);
free_cluster_data:
	if (q->cluster_data)
		free(q->cluster_data);
free_header:
	if (q->header)
		free(q->header);
free_qcow:
	if (q)
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
	};

	return header;
}

static struct disk_image *qcow1_probe(int fd, bool readonly)
{
	struct disk_image *disk_image;
	struct qcow_l1_table *l1t;
	struct qcow_header *h;
	struct qcow *q;

	q = calloc(1, sizeof(struct qcow));
	if (!q)
		return NULL;

	mutex_init(&q->mutex);
	q->fd = fd;

	l1t = &q->table;

	l1t->root = RB_ROOT;
	INIT_LIST_HEAD(&l1t->lru_list);

	h = q->header = qcow1_read_header(fd);
	if (!h)
		goto free_qcow;

	q->version = QCOW1_VERSION;
	q->cluster_size = 1 << q->header->cluster_bits;
	q->cluster_offset_mask = (1LL << (63 - q->header->cluster_bits)) - 1;

	q->cluster_data = malloc(q->cluster_size);
	if (!q->cluster_data) {
		pr_warning("cluster data malloc error!");
		goto free_header;
	}

	q->cluster_cache = malloc(q->cluster_size);
	if (!q->cluster_cache) {
		pr_warning("cluster cache malloc error!");
		goto free_cluster_data;
	}

	if (qcow_read_l1_table(q) < 0)
		goto free_cluster_cache;

	/*
	 * Do not use mmap use read/write instead
	 */
	if (readonly)
		disk_image = disk_image__new(fd, h->size, &qcow_disk_readonly_ops, DISK_IMAGE_REGULAR);
	else
		disk_image = disk_image__new(fd, h->size, &qcow_disk_ops, DISK_IMAGE_REGULAR);

	if (!disk_image)
		goto free_l1_table;

	disk_image->async = 1;
	disk_image->priv = q;

	return disk_image;

free_l1_table:
	if (q->table.l1_table)
		free(q->table.l1_table);
free_cluster_cache:
	if (q->cluster_cache)
		free(q->cluster_cache);
free_cluster_data:
	if (q->cluster_data)
		free(q->cluster_data);
free_header:
	if (q->header)
		free(q->header);
free_qcow:
	if (q)
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

struct disk_image *qcow_probe(int fd, bool readonly)
{
	if (qcow1_check_image(fd))
		return qcow1_probe(fd, readonly);

	if (qcow2_check_image(fd))
		return qcow2_probe(fd, readonly);

	return NULL;
}
