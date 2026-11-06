#!/bin/bash
# Build the thor kernel (Image + DTBs) on macOS with the NDK LLVM toolchain.
# Everything needed for the first boot is built into the Image (no modules
# deployment), so no depmod is required on the host.
set -e

ROOT="/Users/xlisp/PyPro/xiaomi_install_ubuntu"
KERNEL="$ROOT/src/linux-diting"
BUILD="$ROOT/thor-build"
NDKBIN="$ROOT/downloads/android-ndk-r30/toolchains/llvm/prebuilt/darwin-x86_64/bin"
FRAGMENT="$ROOT/thor-port/thor.config"
JOBS=$(( $(sysctl -n hw.ncpu) + 1 ))

export PATH="$NDKBIN:/usr/local/opt/make/libexec/gnubin:$PATH"
export LC_ALL=C
export CCACHE_DISABLE=1

# HOSTCC must be Apple's clang (the NDK clang has no macOS SDK headers);
# target objects are built with the NDK clang via LLVM=1.
KMAKE="make -C $KERNEL O=$BUILD ARCH=arm64 LLVM=1 LLVM_IAS=1 \
	HOSTCC=/usr/bin/clang HOSTCXX=/usr/bin/clang++ -j$JOBS"

mkdir -p "$BUILD"

echo "== toolchain =="
clang --version | head -1
ld.lld --version | head -1

echo "== defconfig + fragment =="
$KMAKE defconfig
$KERNEL/scripts/kconfig/merge_config.sh -m -O "$BUILD" \
	"$BUILD/.config" "$FRAGMENT"
$KMAKE olddefconfig

# Verify every fragment option survived (like the Diting build).
missing=0
while IFS= read -r line; do
	case "$line" in
	CONFIG_*=*) grep -qxF "$line" "$BUILD/.config" || { echo "LOST: $line"; missing=1; } ;;
	"# CONFIG_"*" is not set") grep -qxF "$line" "$BUILD/.config" || { echo "LOST: $line"; missing=1; } ;;
	esac
done < "$FRAGMENT"
if [ "$missing" = 1 ]; then
	echo "ERROR: config fragment not fully applied"
	exit 1
fi

echo "== build =="
$KMAKE Image dtbs modules

echo "== results =="
ls -la "$BUILD/arch/arm64/boot/Image" || exit 1
ls -la "$BUILD/arch/arm64/boot/dts/qcom/sm8475-xiaomi-thor.dtb" || exit 1
echo "OK: Image + sm8475-xiaomi-thor.dtb built"
