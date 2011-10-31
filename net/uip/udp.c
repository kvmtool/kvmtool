#include "kvm/uip.h"

#include <linux/virtio_net.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>

#define UIP_UDP_MAX_EVENTS 1000

static struct uip_udp_socket *uip_udp_socket_find(struct uip_tx_arg *arg, u32 sip, u32 dip, u16 sport, u16 dport)
{
	struct list_head *sk_head;
	struct uip_udp_socket *sk;
	pthread_mutex_t *sk_lock;
	struct epoll_event ev;
	int flags;
	int ret;

	sk_head = &arg->info->udp_socket_head;
	sk_lock = &arg->info->udp_socket_lock;

	/*
	 * Find existing sk
	 */
	mutex_lock(sk_lock);
	list_for_each_entry(sk, sk_head, list) {
		if (sk->sip == sip && sk->dip == dip && sk->sport == sport && sk->dport == dport) {
			mutex_unlock(sk_lock);
			return sk;
		}
	}
	mutex_unlock(sk_lock);

	/*
	 * Allocate new one
	 */
	sk = malloc(sizeof(*sk));
	memset(sk, 0, sizeof(*sk));

	sk->lock = sk_lock;

	sk->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sk->fd < 0)
		goto out;

	/*
	 * Set non-blocking
	 */
	flags = fcntl(sk->fd, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(sk->fd, F_SETFL, flags);

	/*
	 * Add sk->fd to epoll_wait
	 */
	ev.events	= EPOLLIN;
	ev.data.fd	= sk->fd;
	ev.data.ptr	= sk;
	if (arg->info->udp_epollfd <= 0)
		arg->info->udp_epollfd = epoll_create(UIP_UDP_MAX_EVENTS);
	ret = epoll_ctl(arg->info->udp_epollfd, EPOLL_CTL_ADD, sk->fd, &ev);
	if (ret == -1)
		pr_warning("epoll_ctl error");

	sk->addr.sin_family	 = AF_INET;
	sk->addr.sin_addr.s_addr = dip;
	sk->addr.sin_port	 = dport;

	sk->sip			 = sip;
	sk->dip			 = dip;
	sk->sport		 = sport;
	sk->dport		 = dport;

	mutex_lock(sk_lock);
	list_add_tail(&sk->list, sk_head);
	mutex_unlock(sk_lock);

	return sk;

out:
	free(sk);
	return NULL;
}

static int uip_udp_socket_send(struct uip_udp_socket *sk, struct uip_udp *udp)
{
	int len;
	int ret;

	len = ntohs(udp->len) - uip_udp_hdrlen(udp);

	ret = sendto(sk->fd, udp->payload, len, 0, (struct sockaddr *)&sk->addr, sizeof(sk->addr));
	if (ret != len)
		return -1;

	return 0;
}

int uip_udp_make_pkg(struct uip_info *info, struct uip_udp_socket *sk, struct uip_buf *buf, u8* payload, int payload_len)
{
	struct uip_eth *eth2;
	struct uip_udp *udp2;
	struct uip_ip *ip2;

	/*
	 * Cook a ethernet frame
	 */
	udp2		= (struct uip_udp *)(buf->eth);
	eth2		= (struct uip_eth *)buf->eth;
	ip2		= (struct uip_ip *)(buf->eth);

	eth2->src	= info->host_mac;
	eth2->dst	= info->guest_mac;
	eth2->type	= htons(UIP_ETH_P_IP);

	ip2->vhl	= UIP_IP_VER_4 | UIP_IP_HDR_LEN;
	ip2->tos	= 0;
	ip2->id		= 0;
	ip2->flgfrag	= 0;
	ip2->ttl	= UIP_IP_TTL;
	ip2->proto	= UIP_IP_P_UDP;
	ip2->csum	= 0;

	ip2->sip	= sk->dip;
	ip2->dip	= sk->sip;
	udp2->sport	= sk->dport;
	udp2->dport	= sk->sport;

	udp2->len	= htons(payload_len + uip_udp_hdrlen(udp2));
	udp2->csum	= 0;

	if (payload)
		memcpy(udp2->payload, payload, payload_len);

	ip2->len	= udp2->len + htons(uip_ip_hdrlen(ip2));
	ip2->csum	= uip_csum_ip(ip2);
	udp2->csum	= uip_csum_udp(udp2);

	/*
	 * virtio_net_hdr
	 */
	buf->vnet_len	= sizeof(struct virtio_net_hdr);
	memset(buf->vnet, 0, buf->vnet_len);

	buf->eth_len	= ntohs(ip2->len) + uip_eth_hdrlen(&ip2->eth);

	return 0;
}

static void *uip_udp_socket_thread(void *p)
{
	struct epoll_event events[UIP_UDP_MAX_EVENTS];
	struct uip_udp_socket *sk;
	struct uip_info *info;
	struct uip_buf *buf;
	int payload_len;
	u8 *payload;
	int nfds;
	int i;

	info = p;

	do {
		payload = malloc(UIP_MAX_UDP_PAYLOAD);
	} while (!payload);

	while (1) {
		nfds = epoll_wait(info->udp_epollfd, events, UIP_UDP_MAX_EVENTS, -1);

		if (nfds == -1)
			continue;

		for (i = 0; i < nfds; i++) {

			sk = events[i].data.ptr;
			payload_len = recvfrom(sk->fd, payload, UIP_MAX_UDP_PAYLOAD, 0, NULL, NULL);
			if (payload_len < 0)
				continue;

			/*
			 * Get free buffer to send data to guest
			 */
			buf = uip_buf_get_free(info);

			uip_udp_make_pkg(info, sk, buf, payload, payload_len);

			/*
			 * Send data received from socket to guest
			 */
			uip_buf_set_used(info, buf);
		}
	}

	free(payload);
	pthread_exit(NULL);
	return NULL;
}

int uip_tx_do_ipv4_udp(struct uip_tx_arg *arg)
{
	struct uip_udp_socket *sk;
	struct uip_info *info;
	struct uip_udp *udp;
	struct uip_ip *ip;
	int ret;

	udp	= (struct uip_udp *)(arg->eth);
	ip	= (struct uip_ip *)(arg->eth);
	info	= arg->info;

	if (uip_udp_is_dhcp(udp)) {
		uip_tx_do_ipv4_udp_dhcp(arg);
		return 0;
	}

	/*
	 * Find socket we have allocated before, otherwise allocate one
	 */
	sk = uip_udp_socket_find(arg, ip->sip, ip->dip, udp->sport, udp->dport);
	if (!sk)
		return -1;

	/*
	 * Send out UDP data to remote host
	 */
	ret = uip_udp_socket_send(sk, udp);
	if (ret)
		return -1;

	if (!info->udp_thread)
		pthread_create(&info->udp_thread, NULL, uip_udp_socket_thread, (void *)info);

	return 0;
}
