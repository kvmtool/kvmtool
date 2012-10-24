#include "kvm/mutex.h"
#include "kvm/uip.h"

#include <linux/virtio_net.h>
#include <linux/kernel.h>
#include <linux/list.h>

int uip_tx(struct iovec *iov, u16 out, struct uip_info *info)
{
	struct virtio_net_hdr *vnet;
	struct uip_tx_arg arg;
	int eth_len, vnet_len;
	struct uip_eth *eth;
	u8 *buf = NULL;
	u16 proto;
	int i;

	/*
	 * Buffer from guest to device
	 */
	vnet_len = iov[0].iov_len;
	vnet	 = iov[0].iov_base;

	eth_len	 = iov[1].iov_len;
	eth	 = iov[1].iov_base;

	/*
	 * In case, ethernet frame is in more than one iov entry.
	 * Copy iov buffer into one linear buffer.
	 */
	if (out > 2) {
		eth_len = 0;
		for (i = 1; i < out; i++)
			eth_len += iov[i].iov_len;

		buf = malloc(eth_len);
		if (!buf)
			return -1;

		eth = (struct uip_eth *)buf;
		for (i = 1; i < out; i++) {
			memcpy(buf, iov[i].iov_base, iov[i].iov_len);
			buf += iov[i].iov_len;
		}
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
	default:
		break;
	}

	if (out > 2 && buf)
		free(eth);

	return vnet_len + eth_len;
}

int uip_rx(struct iovec *iov, u16 in, struct uip_info *info)
{
	struct virtio_net_hdr *vnet;
	struct uip_eth *eth;
	struct uip_buf *buf;
	int vnet_len;
	int eth_len;
	char *p;
	int len;
	int cnt;
	int i;

	/*
	 * Sleep until there is a buffer for guest
	 */
	buf = uip_buf_get_used(info);

	/*
	 * Fill device to guest buffer, vnet hdr fisrt
	 */
	vnet_len = iov[0].iov_len;
	vnet = iov[0].iov_base;
	if (buf->vnet_len > vnet_len) {
		len = -1;
		goto out;
	}
	memcpy(vnet, buf->vnet, buf->vnet_len);

	/*
	 * Then, the real eth data
	 * Note: Be sure buf->eth_len is not bigger than the buffer len that guest provides
	 */
	cnt = buf->eth_len;
	p = buf->eth;
	for (i = 1; i < in; i++) {
		eth_len = iov[i].iov_len;
		eth = iov[i].iov_base;
		if (cnt > eth_len) {
			memcpy(eth, p, eth_len);
			cnt -= eth_len;
			p += eth_len;
		} else {
			memcpy(eth, p, cnt);
			cnt -= cnt;
			break;
		}
	}

	if (cnt) {
		pr_warning("uip_rx error");
		len = -1;
		goto out;
	}

	len = buf->vnet_len + buf->eth_len;

out:
	uip_buf_set_free(info, buf);
	return len;
}

int uip_init(struct uip_info *info)
{
	struct list_head *udp_socket_head;
	struct list_head *tcp_socket_head;
	struct list_head *buf_head;
	struct uip_buf *buf;
	int buf_nr;
	int i;

	udp_socket_head	= &info->udp_socket_head;
	tcp_socket_head	= &info->tcp_socket_head;
	buf_head	= &info->buf_head;
	buf_nr		= info->buf_nr;

	INIT_LIST_HEAD(udp_socket_head);
	INIT_LIST_HEAD(tcp_socket_head);
	INIT_LIST_HEAD(buf_head);

	mutex_init(&info->udp_socket_lock);
	mutex_init(&info->tcp_socket_lock);
	mutex_init(&info->buf_lock);

	pthread_cond_init(&info->buf_used_cond, NULL);
	pthread_cond_init(&info->buf_free_cond, NULL);


	for (i = 0; i < buf_nr; i++) {
		buf = malloc(sizeof(*buf));
		memset(buf, 0, sizeof(*buf));

		buf->status	= UIP_BUF_STATUS_FREE;
		buf->info	= info;
		buf->id		= i;
		list_add_tail(&buf->list, buf_head);
	}

	list_for_each_entry(buf, buf_head, list) {
		buf->vnet	= malloc(sizeof(struct virtio_net_hdr));
		buf->vnet_len	= sizeof(struct virtio_net_hdr);
		buf->eth	= malloc(1024*64 + sizeof(struct uip_pseudo_hdr));
		buf->eth_len	= 1024*64 + sizeof(struct uip_pseudo_hdr);

		memset(buf->vnet, 0, buf->vnet_len);
		memset(buf->eth, 0, buf->eth_len);
	}

	info->buf_free_nr = buf_nr;
	info->buf_used_nr = 0;

	uip_dhcp_get_dns(info);

	return 0;
}
