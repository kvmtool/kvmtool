#ifndef KVM__IOEVENTFD_H
#define KVM__IOEVENTFD_H

#include <linux/types.h>
#include <linux/list.h>
#include <sys/eventfd.h>
#include "kvm/util.h"

struct kvm;

struct ioevent {
	u64			io_addr;
	u8			io_len;
	void			(*fn)(struct kvm *kvm, void *ptr);
	struct kvm		*fn_kvm;
	void			*fn_ptr;
	int			fd;
	u64			datamatch;

	struct list_head	list;
};

int ioeventfd__init(struct kvm *kvm);
int ioeventfd__exit(struct kvm *kvm);
int ioeventfd__add_event(struct ioevent *ioevent, bool is_pio, bool poll_in_userspace);
int ioeventfd__del_event(u64 addr, u64 datamatch);

#endif
