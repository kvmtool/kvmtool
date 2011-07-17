#include "kvm/uip.h"

#include <arpa/inet.h>

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

int uip_dhcp_get_dns(struct uip_info *info)
{
	char key[256], val[256];
	struct in_addr addr;
	int ret = -1;
	int n = 0;
	FILE *fp;
	u32 ip;

	fp = fopen("/etc/resolv.conf", "r");
	if (!fp)
		goto out;

	while (!feof(fp)) {
		if (fscanf(fp, "%s %s\n", key, val) != 2)
			continue;
		if (strncmp("domain", key, 6) == 0)
			info->domain_name = strndup(val, UIP_DHCP_MAX_DOMAIN_NAME_LEN);
		else if (strncmp("nameserver", key, 10) == 0) {
			if (!inet_aton(val, &addr))
				continue;
			ip = ntohl(addr.s_addr);
			if (n < UIP_DHCP_MAX_DNS_SERVER_NR)
				info->dns_ip[n++] = ip;
			ret = 0;
		}
	}

out:
	fclose(fp);
	return ret;
}
