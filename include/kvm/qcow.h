#ifndef KVM__QCOW_H
#define KVM__QCOW_H

#include <linux/types.h>

#define QCOW_MAGIC		(('Q' << 24) | ('F' << 16) | ('I' << 8) | 0xfb)
#define QCOW1_VERSION		1

#define QCOW_OFLAG_COMPRESSED	(1LL << 63)

struct qcow_table {
	u32			table_size;
	u64			*l1_table;
};

struct qcow {
	void			*header;
	struct qcow_table	table;
	int			fd;
};

struct qcow1_header {
	u32			magic;
	u32			version;

	u64			backing_file_offset;
	u32 			backing_file_size;
	u32			mtime;

	u64			size; /* in bytes */

	u8			cluster_bits;
	u8			l2_bits;
	u32			crypt_method;

	u64			l1_table_offset;
};

struct disk_image *qcow_probe(int fd);

#endif /* KVM__QCOW_H */
