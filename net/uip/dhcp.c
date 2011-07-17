#include "kvm/uip.h"

bool uip_udp_is_dhcp(struct uip_udp *udp)
{
	struct uip_dhcp *dhcp;

	if (ntohs(udp->sport) != UIP_DHCP_PORT_CLIENT ||
	    ntohs(udp->dport) != UIP_DHCP_PORT_SERVER)
		return false;

	dhcp = (struct uip_dhcp *)udp;

	if (ntohl(dhcp->magic_cookie) != UIP_DHCP_MAGIC_COOKIE)
		return false;

	return true;
}
