#include "kvm/uip.h"

static u16 uip_csum(u16 csum, u8 *addr, u16 count)
{
	long sum = csum;

	while (count > 1) {
		sum	+= *(u16 *)addr;
		addr	+= 2;
		count	-= 2;
	}

	if (count > 0)
		sum += *(unsigned char *)addr;

	while (sum>>16)
		sum = (sum & 0xffff) + (sum >> 16);

	return ~sum;
}

u16 uip_csum_ip(struct uip_ip *ip)
{
	return uip_csum(0, &ip->vhl, uip_ip_hdrlen(ip));
}

u16 uip_csum_icmp(struct uip_icmp *icmp)
{
	struct uip_ip *ip;

	ip = &icmp->ip;
	return icmp->csum = uip_csum(0, &icmp->type, htons(ip->len) - uip_ip_hdrlen(ip) - 8); /* icmp header len = 8 */
}

u16 uip_csum_udp(struct uip_udp *udp)
{
	struct uip_pseudo_hdr hdr;
	struct uip_ip *ip;
	int udp_len;
	u8 *pad;

	ip	  = &udp->ip;

	hdr.sip   = ip->sip;
	hdr.dip	  = ip->dip;
	hdr.zero  = 0;
	hdr.proto = ip->proto;
	hdr.len   = udp->len;

	udp_len	  = uip_udp_len(udp);

	if (udp_len % 2) {
		pad = (u8 *)&udp->sport + udp_len;
		*pad = 0;
		memcpy((u8 *)&udp->sport + udp_len + 1, &hdr, sizeof(hdr));
		return uip_csum(0, (u8 *)&udp->sport, udp_len + 1 + sizeof(hdr));
	} else {
		memcpy((u8 *)&udp->sport + udp_len, &hdr, sizeof(hdr));
		return uip_csum(0, (u8 *)&udp->sport, udp_len + sizeof(hdr));
	}

}

u16 uip_csum_tcp(struct uip_tcp *tcp)
{
	struct uip_pseudo_hdr hdr;
	struct uip_ip *ip;
	u16 tcp_len;
	u8 *pad;

	ip	  = &tcp->ip;
	tcp_len   = ntohs(ip->len) - uip_ip_hdrlen(ip);

	hdr.sip   = ip->sip;
	hdr.dip	  = ip->dip;
	hdr.zero  = 0;
	hdr.proto = ip->proto;
	hdr.len   = htons(tcp_len);

	if (tcp_len > UIP_MAX_TCP_PAYLOAD + 20)
		pr_warning("tcp_len(%d) is too large", tcp_len);

	if (tcp_len % 2) {
		pad = (u8 *)&tcp->sport + tcp_len;
		*pad = 0;
		memcpy((u8 *)&tcp->sport + tcp_len + 1, &hdr, sizeof(hdr));
		return uip_csum(0, (u8 *)&tcp->sport, tcp_len + 1 + sizeof(hdr));
	} else {
		memcpy((u8 *)&tcp->sport + tcp_len, &hdr, sizeof(hdr));
		return uip_csum(0, (u8 *)&tcp->sport, tcp_len + sizeof(hdr));
	}
}
