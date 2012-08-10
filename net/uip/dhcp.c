#include "kvm/uip.h"

#include <arpa/inet.h>

#define EMPTY_ADDR "0.0.0.0"

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
		return ret;

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

	fclose(fp);
	return ret;
}

static int uip_dhcp_fill_option_name_and_server(struct uip_info *info, u8 *opt, int i)
{
	u8 domain_name_len;
	u32 *addr;
	int n;

	if (info->domain_name) {
		domain_name_len	= strlen(info->domain_name);
		opt[i++]	= UIP_DHCP_TAG_DOMAIN_NAME;
		opt[i++]	= domain_name_len;
		memcpy(&opt[i], info->domain_name, domain_name_len);
		i		+= domain_name_len;
	}

	for (n = 0; n < UIP_DHCP_MAX_DNS_SERVER_NR; n++) {
		if (info->dns_ip[n] == 0)
			continue;
		opt[i++]	= UIP_DHCP_TAG_DNS_SERVER;
		opt[i++]	= UIP_DHCP_TAG_DNS_SERVER_LEN;
		addr		= (u32 *)&opt[i];
		*addr		= htonl(info->dns_ip[n]);
		i		+= UIP_DHCP_TAG_DNS_SERVER_LEN;
	}

	return i;
}
static int uip_dhcp_fill_option(struct uip_info *info, struct uip_dhcp *dhcp, int reply_msg_type)
{
	int i = 0;
	u32 *addr;
	u8 *opt;

	opt		= dhcp->option;

	opt[i++]	= UIP_DHCP_TAG_MSG_TYPE;
	opt[i++]	= UIP_DHCP_TAG_MSG_TYPE_LEN;
	opt[i++]	= reply_msg_type;

	opt[i++]	= UIP_DHCP_TAG_SERVER_ID;
	opt[i++]	= UIP_DHCP_TAG_SERVER_ID_LEN;
	addr		= (u32 *)&opt[i];
	*addr		= htonl(info->host_ip);
	i		+= UIP_DHCP_TAG_SERVER_ID_LEN;

	opt[i++]	= UIP_DHCP_TAG_LEASE_TIME;
	opt[i++]	= UIP_DHCP_TAG_LEASE_TIME_LEN;
	addr		= (u32 *)&opt[i];
	*addr		= htonl(UIP_DHCP_LEASE_TIME);
	i		+= UIP_DHCP_TAG_LEASE_TIME_LEN;

	opt[i++]	= UIP_DHCP_TAG_SUBMASK;
	opt[i++]	= UIP_DHCP_TAG_SUBMASK_LEN;
	addr		= (u32 *)&opt[i];
	*addr		= htonl(info->guest_netmask);
	i		+= UIP_DHCP_TAG_SUBMASK_LEN;

	opt[i++]	= UIP_DHCP_TAG_ROUTER;
	opt[i++]	= UIP_DHCP_TAG_ROUTER_LEN;
	addr		= (u32 *)&opt[i];
	*addr		= htonl(info->host_ip);
	i		+= UIP_DHCP_TAG_ROUTER_LEN;

	opt[i++]	= UIP_DHCP_TAG_ROOT;
	opt[i++]	= strlen(EMPTY_ADDR);
	addr		= (u32 *)&opt[i];
	strncpy((void *) addr, EMPTY_ADDR, strlen(EMPTY_ADDR));
	i		+= strlen(EMPTY_ADDR);

	i 		= uip_dhcp_fill_option_name_and_server(info, opt, i);

	opt[i++]	= UIP_DHCP_TAG_END;

	return 0;
}

static int uip_dhcp_make_pkg(struct uip_info *info, struct uip_udp_socket *sk, struct uip_buf *buf, u8 reply_msg_type)
{
	struct uip_dhcp *dhcp;

	dhcp		= (struct uip_dhcp *)buf->eth;

	dhcp->msg_type	= 2;
	dhcp->client_ip	= 0;
	dhcp->your_ip	= htonl(info->guest_ip);
	dhcp->server_ip	= htonl(info->host_ip);
	dhcp->agent_ip	= 0;

	uip_dhcp_fill_option(info, dhcp, reply_msg_type);

	sk->sip		= htonl(info->guest_ip);
	sk->dip		= htonl(info->host_ip);
	sk->sport	= htons(UIP_DHCP_PORT_CLIENT);
	sk->dport	= htons(UIP_DHCP_PORT_SERVER);

	return 0;
}

int uip_tx_do_ipv4_udp_dhcp(struct uip_tx_arg *arg)
{
	struct uip_udp_socket sk;
	struct uip_dhcp *dhcp;
	struct uip_info *info;
	struct uip_buf *buf;
	u8 reply_msg_type;

	dhcp = (struct uip_dhcp *)arg->eth;

	if (uip_dhcp_is_discovery(dhcp))
		reply_msg_type = UIP_DHCP_OFFER;
	else if (uip_dhcp_is_request(dhcp))
		reply_msg_type = UIP_DHCP_ACK;
	else
		return -1;

	buf = uip_buf_clone(arg);
	info = arg->info;

	/*
	 * Cook DHCP pkg
	 */
	uip_dhcp_make_pkg(info, &sk, buf, reply_msg_type);

	/*
	 * Cook UDP pkg
	 */
	uip_udp_make_pkg(info, &sk, buf, NULL, UIP_DHCP_MAX_PAYLOAD_LEN);

	/*
	 * Send data received from socket to guest
	 */
	uip_buf_set_used(info, buf);

	return 0;
}
