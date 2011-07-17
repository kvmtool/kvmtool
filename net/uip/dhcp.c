#include "kvm/uip.h"

static inline bool uip_dhcp_is_discovery(struct uip_dhcp *dhcp)
{
	return (dhcp->option[2] == UIP_DHCP_DISCOVER &&
		dhcp->option[1] == UIP_DHCP_TAG_MSG_TYPE_LEN &&
		dhcp->option[0] == UIP_DHCP_TAG_MSG_TYPE);
}

static inline bool uip_dhcp_is_request(struct uip_dhcp *dhcp)
{
	return (dhcp->option[2] == UIP_DHCP_REQUEST &&
		dhcp->option[1] == UIP_DHCP_TAG_MSG_TYPE_LEN &&
		dhcp->option[0] == UIP_DHCP_TAG_MSG_TYPE);
}

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
