#include "kvm/uip.h"

int uip_tx_do_arp(struct uip_tx_arg *arg)
{
	struct uip_arp *arp, *arp2;
	struct uip_info *info;
	struct uip_buf *buf;

	info = arg->info;
	buf = uip_buf_clone(arg);

	arp	 = (struct uip_arp *)(arg->eth);
	arp2	 = (struct uip_arp *)(buf->eth);

	/*
	 * ARP replay code: 2
	 */
	arp2->op   = htons(0x2);
	arp2->dmac = arp->smac;
	arp2->dip  = arp->sip;

	if (arp->dip == htonl(info->host_ip)) {
		arp2->smac = info->host_mac;
		arp2->sip = htonl(info->host_ip);

		uip_buf_set_used(info, buf);
	}

	return 0;
}
