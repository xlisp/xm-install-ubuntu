#!/bin/bash
# Build the thor kernel (Image + DTBs + modules) on macOS with NDK LLVM.
set -e
ROOT="/Users/xlisp/PyPro/xiaomi_install_ubuntu"
KERNEL="$ROOT/src/linux-diting"
BUILD="$ROOT/thor-build"
NDKBIN="$ROOT/downloads/android-ndk-r30/toolchains/llvm/prebuilt/darwin-x86_64/bin"
FRAGMENT="$ROOT/thor-port/thor.config"
LOG="$ROOT/output/kernel-build.log"
JOBS=$(( $(sysctl -n hw.ncpu) + 1 ))

export PATH="$NDKBIN:/usr/local/opt/make/libexec/gnubin:$PATH"
export LC_ALL=C

# macOS lacks glibc <elf.h>; the NDK sysroot ships a compatible one.
NDKINC="$ROOT/downloads/hostinc"

KMAKE="make -C $KERNEL O=$BUILD ARCH=arm64 LLVM=1 LLVM_IAS=1 \
	HOSTCC=/usr/bin/clang HOSTCXX=/usr/bin/clang++ \
	HOSTCFLAGS=-I$NDKINC -j$JOBS"

mkdir -p "$BUILD" "$ROOT/output"
exec > "$LOG" 2>&1
set -x

echo "== toolchain =="
clang --version | head -1
ld.lld --version | head -1

echo "== defconfig + fragment =="
$KMAKE defconfig
cat "$FRAGMENT" >> "$BUILD/.config"
$KMAKE olddefconfig

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
