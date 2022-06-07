#include "kvm/mutex.h"
#include "kvm/uip.h"

#include <linux/virtio_net.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <kvm/iovec.h>

int uip_tx(struct iovec *iov, u16 out, struct uip_info *info)
{
	void *vnet;
	ssize_t len;
	struct uip_tx_arg arg;
	size_t eth_len, vnet_len;
	struct uip_eth *eth;
	void *vnet_buf = NULL;
	void *eth_buf = NULL;
	size_t iovcount = out;

	u16 proto;

	/*
	 * Buffer from guest to device
	 */
	vnet_len = info->vnet_hdr_len;
	vnet	 = iov[0].iov_base;

	len = iov_size(iov, iovcount);
	if (len <= (ssize_t)vnet_len)
		return -EINVAL;

	/* Try to avoid memcpy if possible */
	if (iov[0].iov_len == vnet_len && out == 2) {
		/* Legacy layout: first descriptor for vnet header */
		eth	= iov[1].iov_base;
		eth_len	= iov[1].iov_len;

	} else if (out == 1) {
		/* Single descriptor */
		eth	= (void *)vnet + vnet_len;
		eth_len	= iov[0].iov_len - vnet_len;

	} else {
		/* Any layout */
		len = vnet_len;
		vnet = vnet_buf = malloc(len);
		if (!vnet)
			return -ENOMEM;

		len = memcpy_fromiovec_safe(vnet_buf, &iov, len, &iovcount);
		if (len)
			goto out_free_buf;

		len = eth_len = iov_size(iov, iovcount);
		eth = eth_buf = malloc(len);
		if (!eth)
			goto out_free_buf;

		len = memcpy_fromiovec_safe(eth_buf, &iov, len, &iovcount);
		if (len)
			goto out_free_buf;
	}

	memset(&arg, 0, sizeof(arg));

	arg.vnet_len = vnet_len;
	arg.eth_len = eth_len;
	arg.info = info;
	arg.vnet = vnet;
	arg.eth = eth;

	/*
	 * Check package type
	 */
	proto = ntohs(eth->type);

	switch (proto) {
	case UIP_ETH_P_ARP:
		uip_tx_do_arp(&arg);
		break;
	case UIP_ETH_P_IP:
		uip_tx_do_ipv4(&arg);
		break;
	}

	free(vnet_buf);
	free(eth_buf);

	return vnet_len + eth_len;

out_free_buf:
	free(vnet_buf);
	free(eth_buf);
	return -EINVAL;
}

int uip_rx(struct iovec *iov, u16 in, struct uip_info *info)
{
	struct uip_buf *buf;
	int len;

	/*
	 * Sleep until there is a buffer for guest
	 */
	buf = uip_buf_get_used(info);

	memcpy_toiovecend(iov, buf->vnet, 0, buf->vnet_len);
	memcpy_toiovecend(iov, buf->eth, buf->vnet_len, buf->eth_len);

	len = buf->vnet_len + buf->eth_len;

	uip_buf_set_free(info, buf);
	return len;
}

void uip_static_init(struct uip_info *info)
{
	struct list_head *udp_socket_head;
	struct list_head *tcp_socket_head;
	struct list_head *buf_head;

	udp_socket_head	= &info->udp_socket_head;
	tcp_socket_head	= &info->tcp_socket_head;
	buf_head	= &info->buf_head;

	INIT_LIST_HEAD(udp_socket_head);
	INIT_LIST_HEAD(tcp_socket_head);
	INIT_LIST_HEAD(buf_head);

	mutex_init(&info->udp_socket_lock);
	mutex_init(&info->tcp_socket_lock);
	mutex_init(&info->buf_lock);

	pthread_cond_init(&info->buf_used_cond, NULL);
	pthread_cond_init(&info->buf_free_cond, NULL);

	info->buf_used_nr = 0;
}

int uip_init(struct uip_info *info)
{
	struct list_head *buf_head;
	struct uip_buf *buf;
	int buf_nr;
	int i;

	buf_head	= &info->buf_head;
	buf_nr		= info->buf_nr;

	for (i = 0; i < buf_nr; i++) {
		buf = malloc(sizeof(*buf));
		memset(buf, 0, sizeof(*buf));

		buf->status	= UIP_BUF_STATUS_FREE;
		buf->info	= info;
		buf->id		= i;
		list_add_tail(&buf->list, buf_head);
	}

	list_for_each_entry(buf, buf_head, list) {
		buf->vnet_len   = info->vnet_hdr_len;
		buf->vnet	= malloc(buf->vnet_len);
		buf->eth_len    = 1024*64 + sizeof(struct uip_pseudo_hdr);
		buf->eth	= malloc(buf->eth_len);

		memset(buf->vnet, 0, buf->vnet_len);
		memset(buf->eth, 0, buf->eth_len);
	}

	info->buf_free_nr = buf_nr;

	uip_dhcp_get_dns(info);

	return 0;
}

void uip_exit(struct uip_info *info)
{
	struct uip_buf *buf, *next;

	uip_udp_exit(info);
	uip_tcp_exit(info);
	uip_dhcp_exit(info);

	list_for_each_entry_safe(buf, next, &info->buf_head, list) {
		free(buf->vnet);
		free(buf->eth);
		list_del(&buf->list);
		free(buf);
	}
	uip_static_init(info);
}
