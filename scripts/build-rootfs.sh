#!/bin/bash
# Build the thor Ubuntu rootfs staging tree (no root needed).
set -e
ROOT="/Users/xlisp/PyPro/xiaomi_install_ubuntu"
STAGING="$ROOT/rootfs-staging"
OVERLAY="$ROOT/thor-port/rootfs-overlay"
BASE="$ROOT/downloads/ubuntu-base-26.04-base-arm64.tar.gz"

SEEDS="systemd systemd-sysv openssh-server iproute2 kmod udev netbase ca-certificates locales sudo nano less"

if [ ! -d "$STAGING" ]; then
	echo "== unpacking base =="
	mkdir -p "$STAGING"
	tar -xzf "$BASE" -C "$STAGING"
fi

echo "== fetching debs =="
python3 "$ROOT/scripts/fetch-debs.py" --staging "$STAGING" --seed "$SEEDS"

echo "== overlay =="
cp -R "$OVERLAY/" "$STAGING/"
chmod 755 "$STAGING"/usr/local/sbin/*
chmod 644 "$STAGING"/etc/{hostname,hosts,fstab,ssh/sshd_config}
chmod 644 "$STAGING"/etc/apt/sources.list.d/ubuntu.list

echo "== symlinks =="
mkdir -p "$STAGING"/etc/systemd/system/multi-user.target.wants
mkdir -p "$STAGING"/etc/systemd/system/getty.target.wants
ln -sf /usr/lib/systemd/system/serial-getty@.service \
	"$STAGING"/etc/systemd/system/getty.target.wants/serial-getty@ttyGS0.service
ln -sf /etc/systemd/system/thor-usb-gadget.service \
	"$STAGING"/etc/systemd/system/multi-user.target.wants/thor-usb-gadget.service
ln -sf /etc/systemd/system/thor-firmware-setup.service \
	"$STAGING"/etc/systemd/system/multi-user.target.wants/thor-firmware-setup.service
ln -sf /etc/systemd/system/first-boot.service \
	"$STAGING"/etc/systemd/system/multi-user.target.wants/first-boot.service

echo "== ssh key =="
for k in "$HOME"/.ssh/id_ed25519.pub "$HOME"/.ssh/id_rsa.pub; do
	if [ -f "$k" ]; then
		mkdir -p "$STAGING"/root/.ssh
		cp "$k" "$STAGING"/root/.ssh/authorized_keys.flash
		chmod 600 "$STAGING"/root/.ssh/authorized_keys.flash
		echo "added $k"
		break
	fi
done

# journald: keep logs small (256MB userdata-headroom not an issue, but tidy)
mkdir -p "$STAGING"/etc/systemd/journald.conf.d
cat > "$STAGING"/etc/systemd/journald.conf.d/thor.conf <<EOF
[Journal]
SystemMaxUse=64M
EOF

echo "== staging ready: $STAGING =="
du -sh "$STAGING"
