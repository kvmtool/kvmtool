#include "kvm/mutex.h"
#include "kvm/uip.h"

#include <linux/virtio_net.h>
#include <linux/kernel.h>
#include <linux/list.h>

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

	pthread_mutex_init(&info->udp_socket_lock, NULL);
	pthread_mutex_init(&info->tcp_socket_lock, NULL);
	pthread_mutex_init(&info->buf_lock, NULL);

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

	return 0;
}
