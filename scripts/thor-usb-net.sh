#!/bin/bash
# Bring up the Mac side of the USB gadget network (172.16.42.1 is the phone)
# and share the Mac's internet via NAT.
#   thor-usb-net.sh up|down|ssh
set -e
PHONE_IF=""
PHONE_IP=172.16.42.1
HOST_IP=172.16.42.100

find_iface() {
	# The gadget shows up as an "Ethernet Gadget"/NCM interface
	for ifc in $(ifconfig -l); do
		desc=$(networksetup -listallhardwareports 2>/dev/null | awk -v i="$ifc" '$0 ~ "Device: "i{f=1} f&&/Hardware Address/{getline; print; exit}' 2>/dev/null)
		if ifconfig "$ifc" 2>/dev/null | grep -q "status: active" &&
		   ifconfig "$ifc" 2>/dev/null | grep -qE "ether "; then
			# heuristic: newly appeared en interface without an IPv4
			if [ -z "$(ipconfig getifaddr "$ifc" 2>/dev/null)" ]; then
				echo "$ifc"; return
			fi
		fi
	done
}

case "${1:-up}" in
up)
	IFACE="${2:-$(find_iface)}"
	[ -n "$IFACE" ] || { echo "no USB network interface found; plug the cable / boot the phone" >&2; exit 1; }
	echo "using interface $IFACE"
	sudo ifconfig "$IFACE" "$HOST_IP/24" up
	# NAT: pf forwarding from the USB net out the default route
	DEF_IF=$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')
	[ -n "$DEF_IF" ] || { echo "no default route" >&2; exit 1; }
	sudo sysctl -w net.inet.ip.forwarding=1 >/dev/null
	cat > /tmp/thor-pf.conf <<EOF
nat on $DEF_IF from 172.16.42.0/24 to any -> ($DEF_IF)
pass all
EOF
	sudo pfctl -d 2>/dev/null || true
	sudo pfctl -f /tmp/thor-pf.conf
	sudo pfctl -e
	echo "phone: $PHONE_IP  (ssh root@$PHONE_IP)   host: $HOST_IP"
	;;
down)
	sudo pfctl -d 2>/dev/null || true
	sudo sysctl -w net.inet.ip.forwarding=0 >/dev/null
	echo "NAT off"
	;;
ssh)
	exec ssh -o StrictHostKeyChecking=no root@$PHONE_IP
	;;
*)
	echo "usage: $0 up|down|ssh [iface]" >&2; exit 2 ;;
esac
