for f in /sys/class/net/*; do
	type=`cat $f/type`
	if [ $type -eq 1 ]; then
		f=${f#/sys/class/net/}

		eval "dhcpcd -A $f 2> /dev/null"
		if [ $? -eq 0 ]; then
			exit
		fi

		eval "dhclient $f 2> /dev/null"
		if [ $? -eq 0 ]; then
			exit
		fi

		ifconfig $f 192.168.33.15
		route add default 192.168.33.1
		echo "nameserver 8.8.8.8" >> /etc/resolv.conf

		exit
	fi
done
