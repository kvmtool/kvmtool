#include "kvm/uip.h"

int uip_tx_do_ipv4(struct uip_tx_arg *arg)
{
	struct uip_ip *ip;

	ip = (struct uip_ip *)(arg->eth);

	if (uip_ip_hdrlen(ip) != 20) {
		pr_warning("IP header length is not 20 bytes");
		return -1;
	}

	switch (ip->proto) {
	case UIP_IP_P_ICMP:
		uip_tx_do_ipv4_icmp(arg);
		break;
	case UIP_IP_P_TCP:
		uip_tx_do_ipv4_tcp(arg);
		break;
	case UIP_IP_P_UDP:
		uip_tx_do_ipv4_udp(arg);
		break;
	default:
		break;
	}

	return 0;
}
