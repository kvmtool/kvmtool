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
