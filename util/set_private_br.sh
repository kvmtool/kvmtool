#!/bin/bash
#
# Author: Amos Kong <kongjianjun@gmail.com>
# Date: Apr 14, 2011
# Description: this script is used to create/delete a private bridge,
# launch a dhcp server on the bridge by dnsmasq.
#
# @ ./set_private_br.sh $bridge_name $subnet_prefix
# @ ./set_private_br.sh vbr0 192.168.33

brname='vbr0'
subnet='192.168.33'

add_br()
{
    echo "add new private bridge: $brname"
    /usr/sbin/brctl addbr $brname
    echo 1 > /proc/sys/net/ipv6/conf/$brname/disable_ipv6
    echo 1 > /proc/sys/net/ipv4/ip_forward
    /usr/sbin/brctl stp $brname on
    /usr/sbin/brctl setfd $brname 0
    ifconfig $brname $subnet.1
    ifconfig $brname up
    # Add forward rule, then guest can access public network
    iptables -t nat -A POSTROUTING -s $subnet.254/24 ! -d $subnet.254/24 -j MASQUERADE
    /etc/init.d/dnsmasq stop
    /etc/init.d/tftpd-hpa stop 2>/dev/null
    dnsmasq --strict-order --bind-interfaces --listen-address $subnet.1 --dhcp-range $subnet.1,$subnet.254 $tftp_cmd
}

del_br()
{
    echo "cleanup bridge setup"
    kill -9 `pgrep dnsmasq|tail -1`
    ifconfig $brname down
    /usr/sbin/brctl delbr $brname
    iptables -t nat -D POSTROUTING -s $subnet.254/24 ! -d $subnet.254/24 -j MASQUERADE
}


if [ $# = 0 ]; then
    del_br 2>/dev/null
    exit
fi
if [ $# > 1 ]; then
    brname="$1"
fi
if [ $# = 2 ]; then
    subnet="$2"
fi
add_br
