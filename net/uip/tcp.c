#include "kvm/uip.h"

#include <linux/virtio_net.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <arpa/inet.h>

static int uip_tcp_socket_close(struct uip_tcp_socket *sk, int how)
{
	shutdown(sk->fd, how);

	if (sk->write_done && sk->read_done) {
		shutdown(sk->fd, SHUT_RDWR);
		close(sk->fd);

		mutex_lock(sk->lock);
		list_del(&sk->list);
		mutex_unlock(sk->lock);

		free(sk);
	}

	return 0;
}

static struct uip_tcp_socket *uip_tcp_socket_find(struct uip_tx_arg *arg, u32 sip, u32 dip, u16 sport, u16 dport)
{
	struct list_head *sk_head;
	pthread_mutex_t *sk_lock;
	struct uip_tcp_socket *sk;

	sk_head = &arg->info->tcp_socket_head;
	sk_lock = &arg->info->tcp_socket_lock;

	mutex_lock(sk_lock);
	list_for_each_entry(sk, sk_head, list) {
		if (sk->sip == sip && sk->dip == dip && sk->sport == sport && sk->dport == dport) {
			mutex_unlock(sk_lock);
			return sk;
		}
	}
	mutex_unlock(sk_lock);

	return NULL;
}

static struct uip_tcp_socket *uip_tcp_socket_alloc(struct uip_tx_arg *arg, u32 sip, u32 dip, u16 sport, u16 dport)
{
	struct list_head *sk_head;
	struct uip_tcp_socket *sk;
	pthread_mutex_t *sk_lock;
	struct uip_tcp *tcp;
	struct uip_ip *ip;
	int ret;

	tcp = (struct uip_tcp *)arg->eth;
	ip = (struct uip_ip *)arg->eth;

	sk_head = &arg->info->tcp_socket_head;
	sk_lock = &arg->info->tcp_socket_lock;

	sk = malloc(sizeof(*sk));
	memset(sk, 0, sizeof(*sk));

	sk->lock			= sk_lock;
	sk->info			= arg->info;

	sk->fd				= socket(AF_INET, SOCK_STREAM, 0);
	sk->addr.sin_family		= AF_INET;
	sk->addr.sin_port		= dport;
	sk->addr.sin_addr.s_addr	= dip;

	pthread_cond_init(&sk->cond, NULL);

	if (ntohl(dip) == arg->info->host_ip)
		sk->addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	ret = connect(sk->fd, (struct sockaddr *)&sk->addr, sizeof(sk->addr));
	if (ret) {
		free(sk);
		return NULL;
	}

	sk->sip		= ip->sip;
	sk->dip		= ip->dip;
	sk->sport	= tcp->sport;
	sk->dport	= tcp->dport;

	mutex_lock(sk_lock);
	list_add_tail(&sk->list, sk_head);
	mutex_unlock(sk_lock);

	return sk;
}

static int uip_tcp_payload_send(struct uip_tcp_socket *sk, u8 flag, u16 payload_len)
{
	struct uip_info *info;
	struct uip_eth *eth2;
	struct uip_tcp *tcp2;
	struct uip_buf *buf;
	struct uip_ip *ip2;

	info		= sk->info;

	/*
	 * Get free buffer to send data to guest
	 */
	buf		= uip_buf_get_free(info);

	/*
	 * Cook a ethernet frame
	 */
	tcp2		= (struct uip_tcp *)buf->eth;
	eth2		= (struct uip_eth *)buf->eth;
	ip2		= (struct uip_ip *)buf->eth;

	eth2->src	= info->host_mac;
	eth2->dst	= info->guest_mac;
	eth2->type	= htons(UIP_ETH_P_IP);

	ip2->vhl	= UIP_IP_VER_4 | UIP_IP_HDR_LEN;
	ip2->tos	= 0;
	ip2->id		= 0;
	ip2->flgfrag	= 0;
	ip2->ttl	= UIP_IP_TTL;
	ip2->proto	= UIP_IP_P_TCP;
	ip2->csum	= 0;
	ip2->sip	= sk->dip;
	ip2->dip	= sk->sip;

	tcp2->sport	= sk->dport;
	tcp2->dport	= sk->sport;
	tcp2->seq	= htonl(sk->seq_server);
	tcp2->ack	= htonl(sk->ack_server);
	/*
	 * Diable TCP options, tcp hdr len equals 20 bytes
	 */
	tcp2->off	= UIP_TCP_HDR_LEN;
	tcp2->flg	= flag;
	tcp2->win	= htons(UIP_TCP_WIN_SIZE);
	tcp2->csum	= 0;
	tcp2->urgent	= 0;

	if (payload_len > 0)
		memcpy(uip_tcp_payload(tcp2), sk->payload, payload_len);

	ip2->len	= htons(uip_tcp_hdrlen(tcp2) + payload_len + uip_ip_hdrlen(ip2));
	ip2->csum	= uip_csum_ip(ip2);
	tcp2->csum	= uip_csum_tcp(tcp2);

	/*
	 * virtio_net_hdr
	 */
	buf->vnet_len	= sizeof(struct virtio_net_hdr);
	memset(buf->vnet, 0, buf->vnet_len);

	buf->eth_len	= ntohs(ip2->len) + uip_eth_hdrlen(&ip2->eth);

	/*
	 * Increase server seq
	 */
	sk->seq_server  += payload_len;

	/*
	 * Send data received from socket to guest
	 */
	uip_buf_set_used(info, buf);

	return 0;
}

static void *uip_tcp_socket_thread(void *p)
{
	struct uip_tcp_socket *sk;
	int len, left, ret;
	u8 *payload, *pos;

	sk = p;

	payload = malloc(UIP_MAX_TCP_PAYLOAD);
	if (!payload)
		goto out;

	while (1) {
		pos = payload;

		ret = read(sk->fd, payload, UIP_MAX_TCP_PAYLOAD);

		if (ret <= 0 || ret > UIP_MAX_TCP_PAYLOAD)
			goto out;

		left = ret;

		while (left > 0) {
			mutex_lock(sk->lock);
			while ((len = sk->guest_acked + sk->window_size - sk->seq_server) <= 0)
				pthread_cond_wait(&sk->cond, sk->lock);
			mutex_unlock(sk->lock);

			sk->payload = pos;
			if (len > left)
				len = left;
			if (len > UIP_MAX_TCP_PAYLOAD)
				len = UIP_MAX_TCP_PAYLOAD;
			left -= len;
			pos += len;

			uip_tcp_payload_send(sk, UIP_TCP_FLAG_ACK, len);
		}
	}

out:
	/*
	 * Close server to guest TCP connection
	 */
	uip_tcp_socket_close(sk, SHUT_RD);

	uip_tcp_payload_send(sk, UIP_TCP_FLAG_FIN | UIP_TCP_FLAG_ACK, 0);
	sk->seq_server += 1;

	sk->read_done = 1;

	free(payload);
	pthread_exit(NULL);

	return NULL;
}

static int uip_tcp_socket_receive(struct uip_tcp_socket *sk)
{
	if (sk->thread == 0)
		return pthread_create(&sk->thread, NULL, uip_tcp_socket_thread, (void *)sk);

	return 0;
}

static int uip_tcp_socket_send(struct uip_tcp_socket *sk, struct uip_tcp *tcp)
{
	int len;
	int ret;
	u8 *payload;

	if (sk->write_done)
		return 0;

	payload = uip_tcp_payload(tcp);
	len = uip_tcp_payloadlen(tcp);

	ret = write(sk->fd, payload, len);
	if (ret != len)
		pr_warning("tcp send error");

	return ret;
}

int uip_tx_do_ipv4_tcp(struct uip_tx_arg *arg)
{
	struct uip_tcp_socket *sk;
	struct uip_tcp *tcp;
	struct uip_ip *ip;
	int ret;

	tcp = (struct uip_tcp *)arg->eth;
	ip = (struct uip_ip *)arg->eth;

	/*
	 * Guest is trying to start a TCP session, let's fake SYN-ACK to guest
	 */
	if (uip_tcp_is_syn(tcp)) {
		sk = uip_tcp_socket_alloc(arg, ip->sip, ip->dip, tcp->sport, tcp->dport);
		if (!sk)
			return -1;

		sk->window_size = ntohs(tcp->win);

		/*
		 * Setup ISN number
		 */
		sk->isn_guest  = uip_tcp_isn(tcp);
		sk->isn_server = uip_tcp_isn_alloc();

		sk->seq_server = sk->isn_server;
		sk->ack_server = sk->isn_guest + 1;
		uip_tcp_payload_send(sk, UIP_TCP_FLAG_SYN | UIP_TCP_FLAG_ACK, 0);
		sk->seq_server += 1;

		/*
		 * Start receive thread for data from remote to guest
		 */
		uip_tcp_socket_receive(sk);

		goto out;
	}

	/*
	 * Find socket we have allocated
	 */
	sk = uip_tcp_socket_find(arg, ip->sip, ip->dip, tcp->sport, tcp->dport);
	if (!sk)
		return -1;

	mutex_lock(sk->lock);
	sk->window_size = ntohs(tcp->win);
	sk->guest_acked = ntohl(tcp->ack);
	pthread_cond_signal(&sk->cond);
	mutex_unlock(sk->lock);

	if (uip_tcp_is_fin(tcp)) {
		if (sk->write_done)
			goto out;

		sk->write_done = 1;
		sk->ack_server += 1;
		uip_tcp_payload_send(sk, UIP_TCP_FLAG_ACK, 0);

		/*
		 * Close guest to server TCP connection
		 */
		uip_tcp_socket_close(sk, SHUT_WR);

		goto out;
	}

	/*
	 * Ignore guest to server frames with zero tcp payload
	 */
	if (uip_tcp_payloadlen(tcp) == 0)
		goto out;

	/*
	 * Sent out TCP data to remote host
	 */
	ret = uip_tcp_socket_send(sk, tcp);
	if (ret < 0)
		return -1;
	/*
	 * Send ACK to guest imediately
	 */
	sk->ack_server += ret;
	uip_tcp_payload_send(sk, UIP_TCP_FLAG_ACK, 0);

out:
	return 0;
}
