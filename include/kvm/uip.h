#ifndef KVM__UIP_H
#define KVM__UIP_H

#include "linux/types.h"
#include "kvm/mutex.h"

#include <netinet/in.h>
#include <sys/uio.h>

#define UIP_BUF_STATUS_FREE	0
#define UIP_BUF_STATUS_INUSE	1
#define UIP_BUF_STATUS_USED	2

struct uip_eth_addr {
	u8 addr[6];
};

struct uip_eth {
	struct uip_eth_addr dst;
	struct uip_eth_addr src;
	u16 type;
} __attribute__((packed));

struct uip_arp {
	struct uip_eth eth;
	u16 hwtype;
	u16 proto;
	u8 hwlen;
	u8 protolen;
	u16 op;
	struct uip_eth_addr smac;
	u32 sip;
	struct uip_eth_addr dmac;
	u32 dip;
} __attribute__((packed));

struct uip_info {
	struct list_head udp_socket_head;
	struct list_head tcp_socket_head;
	pthread_mutex_t udp_socket_lock;
	pthread_mutex_t tcp_socket_lock;
	struct uip_eth_addr guest_mac;
	struct uip_eth_addr host_mac;
	pthread_cond_t buf_free_cond;
	pthread_cond_t buf_used_cond;
	struct list_head buf_head;
	pthread_mutex_t buf_lock;
	pthread_t udp_thread;
	int udp_epollfd;
	int buf_free_nr;
	int buf_used_nr;
	u32 host_ip;
	u32 buf_nr;
};

struct uip_buf {
	struct list_head list;
	struct uip_info *info;
	u32 payload;
	int vnet_len;
	int eth_len;
	int status;
	char *vnet;
	char *eth;
	int id;
};

struct uip_tx_arg {
	struct virtio_net_hdr *vnet;
	struct uip_info *info;
	struct uip_eth *eth;
	int vnet_len;
	int eth_len;
};

int uip_tx_do_arp(struct uip_tx_arg *arg);

struct uip_buf *uip_buf_set_used(struct uip_info *info, struct uip_buf *buf);
struct uip_buf *uip_buf_set_free(struct uip_info *info, struct uip_buf *buf);
struct uip_buf *uip_buf_get_used(struct uip_info *info);
struct uip_buf *uip_buf_get_free(struct uip_info *info);
struct uip_buf *uip_buf_clone(struct uip_tx_arg *arg);

#endif /* KVM__UIP_H */
