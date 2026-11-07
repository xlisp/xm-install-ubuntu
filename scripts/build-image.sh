#!/bin/bash
# Assemble the flashable artifacts: rootfs.img (ext4, populated from the
# staging tree) and boot.img/vendor_boot.img.
set -e
ROOT="/Users/xlisp/PyPro/xiaomi_install_ubuntu"
STAGING="$ROOT/rootfs-staging"
OUT="$ROOT/output"
MKE2FS="/usr/local/opt/e2fsprogs-local/sbin/mke2fs"
IMG="$OUT/rootfs.img"
SIZE=12G

mkdir -p "$OUT"

[ -x "$MKE2FS" ] || { echo "mke2fs missing; brew install e2fsprogs" >&2; exit 1; }
[ -d "$STAGING" ] || { echo "staging missing; run build-rootfs.sh" >&2; exit 1; }

echo "== rootfs.img ($SIZE) =="
rm -f "$IMG"
# -d populates the filesystem from the directory in one shot, no root needed
"$MKE2FS" -t ext4 -b 4096 -d "$STAGING" \
	-L thor-root \
	-E lazy_itable_init=0 \
	"$IMG" "$SIZE"

echo "== boot images =="
chmod +x "$ROOT/thor-port/thor-mkbootimg"
"$ROOT/thor-port/thor-mkbootimg" \
	--kernel "$ROOT/thor-build/arch/arm64/boot/Image" \
	--dtb "$ROOT/thor-build/arch/arm64/boot/dts/qcom/sm8475-xiaomi-thor.dtb" \
	--out "$OUT"

echo "== artifacts =="
ls -lh "$OUT"
