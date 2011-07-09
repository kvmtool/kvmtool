#include "kvm/uip.h"

int uip_tx_do_ipv4_icmp(struct uip_tx_arg *arg)
{
	struct uip_ip *ip, *ip2;
	struct uip_icmp *icmp2;
	struct uip_buf *buf;

	buf		= uip_buf_clone(arg);

	icmp2		= (struct uip_icmp *)(buf->eth);
	ip2		= (struct uip_ip *)(buf->eth);
	ip		= (struct uip_ip *)(arg->eth);

	ip2->sip	= ip->dip;
	ip2->dip	= ip->sip;
	ip2->csum	= 0;
	/*
	 * ICMP reply: 0
	 */
	icmp2->type	= 0;
	icmp2->csum	= 0;
	ip2->csum	= uip_csum_ip(ip2);
	icmp2->csum	= uip_csum_icmp(icmp2);

	uip_buf_set_used(arg->info, buf);

	return 0;
}
