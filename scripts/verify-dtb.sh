#!/bin/bash
# Post-build DTB verification: decompile and check the thor-specific nodes.
set -e
DTB="${1:-/Users/xlisp/PyPro/xiaomi_install_ubuntu/thor-build/arch/arm64/boot/dts/qcom/sm8475-xiaomi-thor.dtb}"
T=$(mktemp -d)
dtc -I dtb -O dts -o "$T/thor.dts" "$DTB" 2>/dev/null

check() {
	if grep -q "$2" "$T/thor.dts"; then echo "PASS: $1"; else echo "FAIL: $1 ($2)"; fi
}

check "model" 'Xiaomi 12S Ultra'
check "panel compatible" 'xiaomi,l1-38-0c-0a'
check "panel reset gpio" 'gpio106'
check "touch irq/reset gpio" 'gpio20'
check "touch on spi" 'st,ftm5-spi'
check "speaker amps i2c" 'cirrus,cs35l41'
check "framebuffer 1440x3200" 'width = <0x5a0>'
check "splash renamed" 'splash_region'
check "ABL msm-id" 'qcom,msm-id'
check "ABL board-id" 'qcom,board-id'
check "wifi QCA6490" 'pci17cb,1103'
check "modem fw name" 'modem.mbn'
check "usb peripheral" 'peripheral'
check "UFS enabled" 'ufs_mem_hc'
check "eusb2 ptn3222" 'nxp,ptn3222'
check "ramoops" 'ramoops'
check "panel vci reg gpio186" 'gpio186'

echo "---"
grep -A2 "panel@0" "$T/thor.dts" | head -6
rm -rf "$T"
