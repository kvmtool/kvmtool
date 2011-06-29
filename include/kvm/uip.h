#ifndef KVM__UIP_H
#define KVM__UIP_H

#include "linux/types.h"
#include "kvm/mutex.h"

#include <netinet/in.h>
#include <sys/uio.h>

#define UIP_BUF_STATUS_FREE	0
#define UIP_BUF_STATUS_INUSE	1
#define UIP_BUF_STATUS_USED	2

#define UIP_ETH_P_IP		0X0800

#define UIP_IP_VER_4		0X40
#define UIP_IP_HDR_LEN		0X05
#define UIP_IP_TTL		0X40
#define UIP_IP_P_UDP		0X11

/*
 * IP package maxium len == 64 KBytes
 * IP header == 20 Bytes
 * UDP header == 8 Bytes
 */
#define UIP_MAX_UDP_PAYLOAD	(64*1024 - 20 -  8 - 1)

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

struct uip_ip {
	struct uip_eth eth;
	u8 vhl;
	u8 tos;
	/*
	 * len = IP hdr +  IP payload
	 */
	u16 len;
	u16 id;
	u16 flgfrag;
	u8 ttl;
	u8 proto;
	u16 csum;
	u32 sip;
	u32 dip;
} __attribute__((packed));

struct uip_icmp {
	struct uip_ip ip;
	u8 type;
	u8 code;
	u16 csum;
	u16 id;
	u16 seq;
} __attribute__((packed));

struct uip_udp {
	/*
	 * FIXME: IP Options (IP hdr len > 20 bytes) are not supported
	 */
	struct uip_ip ip;
	u16 sport;
	u16 dport;
	/*
	 * len = UDP hdr +  UDP payload
	 */
	u16 len;
	u16 csum;
	u8 payload[0];
} __attribute__((packed));

struct uip_tcp {
	/*
	 * FIXME: IP Options (IP hdr len > 20 bytes) are not supported
	 */
	struct uip_ip ip;
	u16 sport;
	u16 dport;
	u32 seq;
	u32 ack;
	u8  off;
	u8  flg;
	u16 win;
	u16 csum;
	u16 urgent;
} __attribute__((packed));

struct uip_pseudo_hdr {
	u32 sip;
	u32 dip;
	u8 zero;
	u8 proto;
	u16 len;
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

struct uip_udp_socket {
	struct sockaddr_in addr;
	struct list_head list;
	pthread_mutex_t *lock;
	u32 dport, sport;
	u32 dip, sip;
	int fd;
};

struct uip_tx_arg {
	struct virtio_net_hdr *vnet;
	struct uip_info *info;
	struct uip_eth *eth;
	int vnet_len;
	int eth_len;
};

static inline u16 uip_ip_hdrlen(struct uip_ip *ip)
{
	return (ip->vhl & 0x0f) * 4;
}

static inline u16 uip_ip_len(struct uip_ip *ip)
{
	return htons(ip->len);
}

static inline u16 uip_udp_hdrlen(struct uip_udp *udp)
{
	return 8;
}

static inline u16 uip_udp_len(struct uip_udp *udp)
{
	return ntohs(udp->len);
}

static inline u16 uip_eth_hdrlen(struct uip_eth *eth)
{
	return sizeof(*eth);
}

int uip_tx_do_ipv4_icmp(struct uip_tx_arg *arg);
int uip_tx_do_ipv4_udp(struct uip_tx_arg *arg);
int uip_tx_do_ipv4(struct uip_tx_arg *arg);
int uip_tx_do_arp(struct uip_tx_arg *arg);

u16 uip_csum_icmp(struct uip_icmp *icmp);
u16 uip_csum_udp(struct uip_udp *udp);
u16 uip_csum_ip(struct uip_ip *ip);

struct uip_buf *uip_buf_set_used(struct uip_info *info, struct uip_buf *buf);
struct uip_buf *uip_buf_set_free(struct uip_info *info, struct uip_buf *buf);
struct uip_buf *uip_buf_get_used(struct uip_info *info);
struct uip_buf *uip_buf_get_free(struct uip_info *info);
struct uip_buf *uip_buf_clone(struct uip_tx_arg *arg);

#endif /* KVM__UIP_H */
