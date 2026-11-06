#!/bin/bash
# Flash the thor Ubuntu images. Run with the phone in fastboot (bootloader)
# mode and the bootloader ALREADY UNLOCKED. Wipes userdata (Android is gone).
#
#   flash-thor.sh            flash boot_b/vendor_boot_b + userdata
#   flash-thor.sh --boot-only    only boot/vendor_boot (kernel iteration)
set -e
ROOT="/Users/xlisp/PyPro/xiaomi_install_ubuntu"
OUT="$ROOT/output"
FB="fastboot"

for f in "$OUT/boot.img" "$OUT/vendor_boot.img"; do
	[ -f "$f" ] || { echo "missing $f; run build-image.sh first" >&2; exit 1; }
done

echo "== device =="
$FB devices
$FB getvar current-slot || true
$FB getvar unlocked || true

echo "== flashing boot images to slot b =="
$FB flash boot_b "$OUT/boot.img"
$FB flash vendor_boot_b "$OUT/vendor_boot.img"
# dtbo must not match the mainline DTB (vendor overlays are incompatible)
$FB erase dtbo_b || true
$FB set_active b

if [ "${1:-}" != "--boot-only" ]; then
	[ -f "$OUT/rootfs.img" ] || { echo "missing $OUT/rootfs.img" >&2; exit 1; }
	echo "== flashing userdata (wipes Android data) =="
	$FB flash userdata "$OUT/rootfs.img"
fi

echo "== done; rebooting =="
$FB reboot
echo "first boot takes a few minutes (firmware extraction + rootfs resize)."
echo "Then on the Mac: $(dirname "$0")/thor-usb-net.sh up && ssh root@172.16.42.1"
