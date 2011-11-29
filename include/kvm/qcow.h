#ifndef KVM__QCOW_H
#define KVM__QCOW_H

#include "kvm/mutex.h"

#include <linux/types.h>
#include <stdbool.h>
#include <linux/rbtree.h>
#include <linux/list.h>

#define QCOW_MAGIC		(('Q' << 24) | ('F' << 16) | ('I' << 8) | 0xfb)

#define QCOW1_VERSION		1
#define QCOW2_VERSION		2

#define QCOW1_OFLAG_COMPRESSED	(1ULL << 63)

#define QCOW2_OFLAG_COPIED	(1ULL << 63)
#define QCOW2_OFLAG_COMPRESSED	(1ULL << 62)

#define QCOW2_OFLAGS_MASK	(QCOW2_OFLAG_COPIED|QCOW2_OFLAG_COMPRESSED)

#define QCOW2_OFFSET_MASK	(~QCOW2_OFLAGS_MASK)

#define MAX_CACHE_NODES         32

struct qcow_l2_table {
	u64				offset;
	struct rb_node			node;
	struct list_head		list;
	u8				dirty;
	u64				table[];
};

struct qcow_l1_table {
	u32				table_size;
	u64				*l1_table;

	/* Level2 caching data structures */
	struct rb_root			root;
	struct list_head		lru_list;
	int				nr_cached;
};

#define QCOW_REFCOUNT_BLOCK_SHIFT	1

struct qcow_refcount_block {
	u64				offset;
	struct rb_node			node;
	struct list_head		list;
	u64				size;
	u8				dirty;
	u16				entries[];
};

struct qcow_refcount_table {
	u32				rf_size;
	u64				*rf_table;

	/* Refcount block caching data structures */
	struct rb_root			root;
	struct list_head		lru_list;
	int				nr_cached;
};

struct qcow_header {
	u64				size;	/* in bytes */
	u64				l1_table_offset;
	u32				l1_size;
	u8				cluster_bits;
	u8				l2_bits;
	u64				refcount_table_offset;
	u32				refcount_table_size;
};

struct qcow {
	pthread_mutex_t			mutex;
	struct qcow_header		*header;
	struct qcow_l1_table		table;
	struct qcow_refcount_table	refcount_table;
	int				fd;
	int				csize_shift;
	int				csize_mask;
	u32				version;
	u64				cluster_size;
	u64				cluster_offset_mask;
	u64				free_clust_idx;
	void				*cluster_cache;
	void				*cluster_data;
	void				*copy_buff;
};

struct qcow1_header_disk {
	u32				magic;
	u32				version;

	u64				backing_file_offset;
	u32 				backing_file_size;
	u32				mtime;

	u64				size;	/* in bytes */

	u8				cluster_bits;
	u8				l2_bits;
	u32				crypt_method;

	u64				l1_table_offset;
};

struct qcow2_header_disk {
	u32				magic;
	u32				version;

	u64				backing_file_offset;
	u32				backing_file_size;

	u32				cluster_bits;
	u64				size;	/* in bytes */
	u32				crypt_method;

	u32				l1_size;
	u64				l1_table_offset;

	u64				refcount_table_offset;
	u32				refcount_table_clusters;

	u32				nb_snapshots;
	u64				snapshots_offset;
};

struct disk_image *qcow_probe(int fd, bool readonly);

#endif /* KVM__QCOW_H */
