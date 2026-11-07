# Xiaomi 12S Ultra（thor）原生 Ubuntu 移植

> 在小米 12S Ultra 上运行**真正的原生 Ubuntu**（apt/debootstrap，非 Ubuntu Touch、
> 非 Halium、非 chroot/proot），基于上游稳定版主线内核 + 自研设备树与面板驱动。
> 参考并复用同 SoC 姊妹机型 Xiaomi 12T Pro（diting）的成熟移植
> [royka1/diting-ubuntu](https://github.com/royka1/diting-ubuntu)。

| 检查项 | 结果 |
| --- | --- |
| 设备 | Xiaomi 12S Ultra（2203121C，代号 `thor`） |
| SoC | Qualcomm SM8475（骁龙 8+ Gen 1，taro 平台） |
| 原系统 | HyperOS 3.0.6.0.VLACNXM（Android 15，中国版 ROM） |
| Bootloader | 初始锁定；需小米社区 App 申请解锁（72h 等待期） |
| 主线内核 | 7.2.10 stable（royka1/linux-diting 树 + thor 移植补丁） |
| 用户态 | Ubuntu 26.04 LTS `resolute` arm64 |
| 工作目录 | `/Users/xlisp/PyPro/xiaomi_install_ubuntu`（macOS 宿主交叉编译） |

## 目录

- [1. 可行性调研结论](#1-可行性调研结论)
- [2. 硬件逆向：从厂商 DTB 提取 thor 板级参数](#2-硬件逆向从厂商-dtb-提取-thor-板级参数)
- [3. 移植架构与产出清单](#3-移植架构与产出清单)
- [4. 面板驱动的诞生过程](#4-面板驱动的诞生过程)
- [5. 设备树 sm8475-xiaomi-thor.dts](#5-设备树-sm8475-xiaomi-thordts)
- [6. 内核配置](#6-内核配置)
- [7. Ubuntu rootfs 的构建](#7-ubuntu-rootfs-的构建)
- [8. boot/vendor_boot 镜像与 ABL 细节](#8-bootvendor_boot-镜像与-abl-细节)
- [9. macOS 交叉编译：踩过的 9 个坑与全部解法](#9-macos-交叉编译踩过的-9-个坑与全部解法)
- [10. 目录结构](#10-目录结构)
- [11. 构建与刷机操作手册](#11-构建与刷机操作手册)
- [12. 当前状态与已知限制](#12-当前状态与已知限制)
- [13. 致谢与参考](#13-致谢与参考)

---

## 1. 可行性调研结论

2026-11 调研（研究记录见对话与 [postmarketOS wiki](https://wiki.postmarketos.org/wiki/Xiaomi_12S_Ultra_(xiaomi-thor))）：

**thor 没有任何现成移植**：

- postmarketOS（现更名 Nura）的 thor 页面是"not booting"占位页，pmaports 无 `device-xiaomi-thor` 包
- 主线内核 `arch/arm64/boot/dts/qcom/` 无任何小米 SM8450/SM8475 手机
- 小米从未发布 12S 系列内核源码（[MiCode issue #4782](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/issues/4782)，甚至删过 mayfly 的源码）
- sm8450-mainline 社区组织有 cupid/zeus/pipa 树，但无 thor

**但 taro 平台 SoC 级支持已成熟**，姊妹机型有完整可用的原生 Ubuntu：

| 机型 | 代号 | 项目 | 完成度 |
| --- | --- | --- | --- |
| 小米 12T Pro | diting（同 SM8475） | royka1/diting-ubuntu（Ubuntu 26.04 + Phosh，内核 7.2.10） | 电话/短信/5G/120Hz/GPU/WiFi/BT/音频全可用 |
| 小米 Pad 6 Pro | liuqin（同 SM8475） | yzddmr6/xiaomipad-6pro-mainline（Ubuntu 26.04 + GNOME） | 平板全功能 |

因此策略确定为：**fork diting 的 SoC 级成果（内核补丁系列 + 构建系统 + 部署知识），
自研 thor 的板级部分（设备树 + 面板驱动 + 配置）**。diting 与 thor 共享：
UFS/USB(eUSB2+PTN3222)/WCN6855 WiFi/QUP 布局/内存 carveout/PMIC 拓扑，差异集中在
面板、触摸固件、扬声器接线与 5 路电压约束。

**Bootloader 解锁（关键路径）**：中国版 HyperOS 3 需小米社区 App 申请（等级+答题），
**每账号每年仅 1 台**，通过后绑定设备等 **72 小时** 才能执行 `fastboot flashing unlock`。
本仓库所有构建工作就是为了等这 72h 期间完成"解锁即可刷"。

## 2. 硬件逆向：从厂商 DTB 提取 thor 板级参数

手机还在 Android 上时，通过 adb 读取 `/sys/firmware/fdt` 的反编译产物
（[sm8450-mainline/fdt](https://github.com/sm8450-mainline/fdt) 的 `thor.dtb`），
用两个自写脚本提取：

- **`scripts/fdt_extract.py`**：基于 fdt 仓库自带 pyfdt，绕开其把裸整数误判为
  phandle 的打印缺陷，输出**原始词值**并正确解析真 phandle 引用
- **`scripts/regdiff.py`**：对比 thor 与 diting 的全部 73 个 rpmh-regulator 叶节点约束

### 决定性发现

1. **真实面板不是 Sharp 节点**。厂商 DTB 的 `chosen/bootargs` 里明确写着
   `msm_drm.dsi_display0=qcom,mdss_dsi_l1_38_0c_0a_dsc_cmd`——真实面板是
   `l1_38_0c_0a`（"xiaomi 38 0c 0a cmd mode dsc dsi panel"），Sharp `sharp_qhd_plus_dsc_cmd`
   只是同文件里的未启用备选。误用 Sharp 的双 DSI 方案会白费功夫（l1 是**单 DSI**）。
2. **面板参数**：1440×3200 LTPO AMOLED，单 DSI 4-lane，cmd 模式，DSC 1.1
   （10bpc/8bpp，slice 720×50×2），LTPO 8 档刷新率（120/90/60/40/30/24/10/1Hz），
   背光走 DCS 0x51（16-bit 大端反相 DBV，min 4 / max 2047），复位 GPIO106。
3. **触摸与 diting 同接线**：STMicro FTM5 在 QUP SE4 `spi@990000`，IRQ TLMM 21 /
   复位 TLMM 20；供电直接用 PMIC 轨（avdd=L9C 3.3V，vdd=L2C 1.8V），
   固件 `st_fts_l1.ftb` 以 10 倍分辨率上报（14400×32000）。
4. **扬声器**：双 Cirrus CS35L41（0x42=B / 0x43=T），I2C15(0x880000) 400kHz，
   复位 TLMM 24/66，中断 TLMM 63/88——diting 同款只是地址不同。
5. **电压约束 diff（12 处，5 路实质差异）**：L3C 2.7V(diting 3.3V)、L7C 3.304V、
   L9C 3.3V 固定、L11C 3.008V 固定、L12C 1.8V 固定；thor 还多一颗
   PM8010 显示 PMIC（7 路 LDO，暂无消费者，未纳入）。
6. **ABL 引导 ID 与 diting 完全一致**：`qcom,msm-id <531 65536 540 65536 530 65536>`、
   `qcom,board-id <65544 0>`、splash 缓冲同为 `0xb8000000 + 0x2b00000`。
7. 内存 carveout（ADSP 0x9fd00000/0x3100000 等）与主线 sm8450.dtsi 默认值一致；
   ramoops 布局同 diting（0xa7000000，2MiB console + 2MiB pmsg），失败启动的
   内核日志可从 Android pstore 读回。

提取的完整原始 DTS 存档在 `backup/thor-raw-dts.txt` 与 `backup/diting-raw-dts.txt`；
厂商命令序列生成的 C 代码存档在 `backup/on60.c` / `backup/on120.c` / `backup/off.c`。

## 3. 移植架构与产出清单

```
Android boot v4 (A/B, header 4, pagesize 4096)
├── boot.img        61M  内核 Image（未压缩 arm64，约 64MB）
└── vendor_boot.img 124K  mainline DTB + cmdline（无 initramfs，内核直挂 rootfs）
userdata (sda36) ─── rootfs.img 12G ext4（首启 resize2fs 扩满分区）
```

| 产出 | 说明 |
| --- | --- |
| `output/boot.img` | mainline Image + 注入 ABL id 的 DTB |
| `output/vendor_boot.img` | DTB 载体，cmdline `console=tty0 earlycon fw_devlink.sync_state=timeout root=/dev/sda36 rw` |
| `output/rootfs.img` | Ubuntu 26.04 arm64，127 包（base 87 + 依赖闭包 40），e2fsck 0 错误，稀疏文件实占 427M |

**关键驱动全部内建**（`=y`）：UFS/QMP PHY/inline crypto、eUSB2+PTN3222+configfs gadget、
DRM/DSI/面板/触摸、CS35L41、CAMCC、ramoops/pstore——首启不依赖 initramfs 和模块加载。

## 4. 面板驱动的诞生过程

`thor-port/panel-xiaomi-l1-38-0c-0a.c`（内核树内同名文件，Makefile/Kconfig/binding 已加）。

厂商 DT 的命令 blob 格式（从 lmdpdg 生成器源码逆向确认）：

```
[dtype 1B][ctrl 1B][vc 1B][flags 1B][wait_ms 1B][dlen BE16][payload...]
```

自写 **`scripts/gen_l1_panel_cmds.py`** 把 `qcom,mdss-dsi-on-command`（60Hz：74 条 / 977B，
120Hz：79 条 / 1027B）与 `off-command` 解析成 `mipi_dsi_dcs_write_seq_multi()` 代码，
再手工整合为带 60/120Hz 双时序分支的完整 DRM 面板驱动：

- 复位时序按厂商 `qcom,mdss-dsi-reset-sequence <0 1 1 10>`（低 1ms → 高 10ms）
- DCS 背光：16-bit 大端 DBV（`qcom,mdss-dsi-bl-inverted-dbv`），max 2047
- DSC：slice 720×50×2，10bpc/8bpp，block prediction；RC 参数复用
  diting/NT37705 级 "nvt" 表（厂商 0x9e 私有配置内嵌 initial_xmit_delay=512 /
  initial_offset=6144，与 diting 完全一致，佐证共享 RC 基础参数）
- 模式：porches 用厂商值（h: 16/32/32，v: 8/24/24）；60Hz preferred；
  `drm_mode_vrefresh()` 判当前时序（新内核已移除 `mode.vrefresh` 字段）

主线 msm DPU/DSI 基础设施对本面板的全部需求（bonded-DSC 拓扑选择、
`dsi_adjust_pclk_for_compression` 的 bonded 分支、命令只发 master 链路）均已原生支持，
无需改 msm 驱动。

## 5. 设备树 sm8475-xiaomi-thor.dts

`thor-port/sm8475-xiaomi-thor.dts`，安装进内核树 `arch/arm64/boot/dts/qcom/`，
Makefile 追加 `sm8475-xiaomi-thor.dtb`。

- 基于 `sm8475.dtsi` + 6 颗 PMIC dtsi；`/delete-node/` 覆盖 adsp/rmtfs/xbl 与 diting 相同
- `regulators-0..3`：电压按 thor 厂商值（见 §2 第 5 条），注释逐条标来源
- 面板：`panel@0` compatible `xiaomi,l1-38-0c-0a`，vddio=L12C，vci=GPIO186 固定稳压
  （`panel_vci` regulator-fixed），复位 GPIO106，TE GPIO86 `mdp_vsync`
- 触摸 `&spi4`：`st,ftm5-spi`，`touchscreen-size-x/y = <14400 32000>`
- 扬声器 `&i2c15`：CS35L41@0x42/0x43，tertiary MI2S（gpio121-124）接 `&sound`
  （`qcom,sm8450-sndcard`，model XIAOMI-THOR）
- WCD938x 编解码 + SoundWire rx/tx（与 diting 相同，耳机口不存在）
- WiFi/BT：`wcn6855-pmu`（GPIO 80/81/82/204）+ `pcieport0` QCA6490
  （`pci17cb,1103`，calibration-variant "Xiaomi_thor"）+ `&uart20` BT
- USB：`dr_mode="peripheral"`，high-speed，eUSB2 PHY + PTN3222@i2c5 0x4f
- chosen framebuffer 1440×3200@0xb8000000；reserved-memory 与厂商一致
- 固件路径约定 `qcom/sm8475/xiaomi/thor/*.mbn`（首启从手机 modem_ 分区自提取，
  qcom_mdt 加载器自动解析 `.mdt`+`bNN` 分段）

DTB 构建后校验：`scripts/verify-dtb.sh` 反编译核对 13 项关键节点/属性（当前 13/13 PASS；
splash 改名与 ABL id 在 mkbootimg 阶段注入，故不在 DTB 校验范围）。

## 6. 内核配置

`thor-port/thor.config`：arm64 defconfig 之上的碎片（构建脚本校验每一项必须存活）。
要点：关键驱动 `=y`（见 §3）；`CONFIG_DRM_MIPI_DSI=y`/`BACKLIGHT_CLASS_DEVICE=y`/
`SOUND`+`SND_SOC`+`CS35L41=y` 父级钉住（否则 defconfig 的 `=m` 会把子选项压成 m）；
Ubuntu 用户态特性（AppArmor、squashfs 全压缩、binderfs/PSI for Waydroid、
nftables/iptables legacy 大全家桶）；pstore/ramoops 调试；FTRACE/KPROBE。

已知取舍：`CONFIG_NETFILTER_XT_TARGET_TCPMSS`、`IP{,6}_NF_TARGET_REJECT` 被
移除——APFS 大小写不敏感卷上 `xt_TCPMSS.c` 与 `xt_tcpmss.c` 等撞名文件无法共存
（见 §9 第 8 条）。Waydroid netd 的 TCPMSS 钳制/REJECT 规则缺失，属可接受限制。

## 7. Ubuntu rootfs 的构建

macOS 无 root、无 Docker 的前提下自研管线（`scripts/build-rootfs.sh` +
`scripts/fetch-debs.py`，一个 230 行的 mini-debootstrap）：

1. **基底**：Ubuntu 官方 `ubuntu-base-26.04-base-arm64.tar.gz`（TUNA 镜像，87 包，
   仅含 apt/dpkg/e2fsprogs——**连 systemd 都没有**）
2. **依赖解析**：fetch-debs.py 拉 TUNA ubuntu-ports 的 `Packages.gz` 索引，
   以 `systemd systemd-sysv openssh-server iproute2 kmod udev netbase
   ca-certificates locales sudo nano less` 为种子做 BFS 依赖解析（含 Provides），
   下载 .deb 后 `ar x` + 解包 `data.tar`（macOS bsdtar 透明解 .zst/.xz），
   **把 control 注册进 dpkg status 数据库**（设备上 apt 视其为已装）
3. **overlay**（`thor-port/rootfs-overlay/`）：
   - `thor-usb-gadget`（configfs NCM+ACM，手机侧 172.16.42.1/24）+ systemd 服务
   - `thor-first-boot`：chown 树归 root（镜像在 macOS 构建，inode 属主是构建用户）→
     `resize2fs` 扩满分区 → 生成 machine-id → `ssh-keygen -A` → 启用预置公钥
   - `thor-firmware-setup`：只读挂载 `modem_$slot` FAT 分区，复制全部 `.mdt`+`bNN`
     到 `/lib/firmware/qcom/sm8475/xiaomi/thor/`（主线 qcom_mdt 加载器按
     `.mbn` 名自动回退解析分段文件）——不需要 diting 那套 erofs/dm 提取
   - `/etc/fstab`（sda36）、静态 `resolv.conf`（223.5.5.5 / 119.29.29.29 走 Mac NAT）、
     apt 源指 TUNA、sshd_config（StrictModes no + 仅公钥）、serial-getty@ttyGS0
4. **打包**：`scripts/build-image.sh` 用自编译的 e2fsprogs `mke2fs -t ext4 -d` 一次
   成像（免 root 免挂载），`e2fsck -fn` 校验，`debugfs` 抽查属主/内容

属主策略：镜像内文件 uid=501（macOS 无法 sudo 归零），**设备端首启 chown 回 root**，
且 first-boot.service 先于 ssh（drop-in `After=first-boot.service`），SSHD StrictModes 关闭。

## 8. boot/vendor_boot 镜像与 ABL 细节

`thor-port/thor-mkbootimg`（移植自 diting 的 mkbootimg 包装，去 initramfs 化）：

1. 校验内核是未压缩 arm64 Image（魔数 `ARMd`，偏移 56）
2. `fdtput` 注入 `qcom,msm-id` / `qcom,board-id`（上游弃用属性，ABL 选 DTB 仍依赖）
3. **splash 区改名**：`/reserved-memory/memory@b8000000` → `splash_region` +
   `label=cont_splash_region`——否则 ABL 日志 "Splash region not found" 并**直接关屏**，
   simple-framebuffer 全黑
4. `mkbootimg.py`（AOSP，jbeich 镜像下载 + gki 导入免死补丁）组 header v4 镜像，
   dtb_offset 0x1f00000，尺寸守卫 boot 192M / vendor_boot 96M

`scripts/flash-thor.sh`：`flash boot_b vendor_boot_b` + `erase dtbo_b`（厂商 overlay
不能叠在 mainline DTB 上）+ `set_active b` + `flash userdata rootfs.img`。

## 9. macOS 交叉编译：踩过的 9 个坑与全部解法

宿主：Intel macOS 15.8（x86_64），无 Docker。工具链：Android NDK r30 clang 21
（`--target=aarch64-linux-gnu`），HOSTCC 固定 Apple clang（NDK clang 无 SDK 头）。

| # | 坑 | 解法 |
| --- | --- | --- |
| 1 | 系统 make 3.81，内核要求 ≥4.0 | `brew install make`，PATH 用 gnubin |
| 2 | NDK clang 编 HOST 工具缺 `sys/types.h` | `HOSTCC=/usr/bin/clang`（目标码仍 NDK clang） |
| 3 | `merge_config.sh` 用 GNU `readlink -m` | 废用之：直接 `cat fragment >> .config` + `olddefconfig`，构建脚本自带逐行存活校验 |
| 4 | defconfig 的 `DRM/SND=m` 把子选项压级（`LOST` 校验报错） | 片段显式钉 `DRM_MIPI_DSI=y`、`SND_SOC_CS35L41=y` 等父级 |
| 5 | 无 `<elf.h>` | `downloads/hostinc/` shim：NDK bionic elf.h 依赖链太深且 `ELF64_ST_TYPE` 递归悬空，最终以 jsdelivr 拉取 **glibc 4483 行 elf.h** + 自写 `features.h` 兜底；`HOSTCFLAGS=-I…/hostinc` |
| 6 | modpost 缺 `byteswap.h` / macOS `uuid_t` 撞内核 typedef | byteswap 用 clang builtin 三行实现；`scripts/mod/file2alias.c` 本地补丁 `#define uuid_t uuid_kernel_t` |
| 7 | gen-kernel-hwcaps.sh 的 BSD sed 不支持 GNU `\+` → `KERNEL_HWCAP_*` 全空 → asm-offsets 编译失败 | `brew install gnu-sed`，`downloads/hostbin/sed` 符号链接注入 PATH（第 4 次定位到的隐藏最深的坑） |
| 8 | APFS 大小写不敏感：`xt_TCPMSS.c`/`xt_tcpmss.c`、`ipt_REJECT.c` 等撞名丢失 | 从配置碎片移除 3 个目标（见 §6 取舍） |
| 9 | 内核源码在 clang 21 下的严格性：q6apm `__free(cleanup)` 声明跨 goto（gcc 容忍 clang 拒绝）、面板新 API（`mode.vrefresh` 已移除）、`gen_init_cpio` 缺 O_LARGEFILE/copy_file_range | q6apm 声明提升到函数顶（NULL 安全）；面板改 `drm_mode_vrefresh()`；gen_init_cpio 加宏守卫 + read/write 回退实现 |

配套工具链：`brew` 的 dtc（fdtput/fdtget 注入）、自编译 e2fsprogs（brew 屡次网络
失败后改从 GitHub tytso/e2fsprogs v1.47.2 源码 `./configure && make`，
装到 `/usr/local/opt/e2fsprogs-local/`）。

构建脚本（幂等，全量重建 ≈ 40-60 分钟，增量秒级）：

- **`scripts/build-kernel-macos.sh`**：defconfig → 碎片合并+校验 → `Image dtbs modules`
  → 产出 `thor-build/arch/arm64/boot/{Image, dts/qcom/sm8475-xiaomi-thor.dtb}`，
  日志 `output/kernel-build.log`
- **`scripts/build-rootfs.sh`** → **`scripts/build-image.sh`**：staging → rootfs.img →
  boot/vendor_boot 镜像

## 10. 目录结构

```
xiaomi_install_ubuntu/
├── README.md                    本文档
├── thor-port/                   ★ 全部自研移植产物（进内核树/打包的源）
│   ├── panel-xiaomi-l1-38-0c-0a.c   面板驱动（DRM panel）
│   ├── xiaomi,l1-38-0c-0a.yaml      DT binding
│   ├── sm8475-xiaomi-thor.dts       板级设备树
│   ├── thor.config                  内核配置碎片
│   ├── thor-mkbootimg               boot 镜像组装（ABL 注入）
│   └── rootfs-overlay/              设备端 overlay（systemd 单元/首启脚本/配置）
├── scripts/                     构建与运维管线
│   ├── build-kernel-macos.sh    内核交叉编译（含全部 shim 注入）
│   ├── build-rootfs.sh          rootfs staging（调 fetch-debs）
│   ├── fetch-debs.py            mini-debootstrap（依赖解析/下载/解包/dpkg 注册）
│   ├── build-image.sh           mke2fs 成像 + mkbootimg
│   ├── gen_l1_panel_cmds.py     厂商命令 blob → C 代码转换器
│   ├── fdt_extract.py           厂商 DTB 原始字段提取
│   ├── regdiff.py               两机 regulator 约束 diff
│   ├── verify-dtb.sh            DTB 构建 13 项校验
│   ├── flash-thor.sh            fastboot 刷机（boot_b/vendor_boot_b/dtbo/userdata）
│   └── thor-usb-net.sh          Mac 侧 USB 网卡 up/down/NAT/ssh
├── output/                      ★ 可刷产物（boot.img / vendor_boot.img / rootfs.img / 构建日志）
├── backup/                      原始 DTS 反编译存档 + 生成的面板命令 C 代码
├── downloads/                   NDK r30、Ubuntu base tar、mkbootimg.py、hostinc shim、hostbin(gnu-sed)、deb 缓存
├── rootfs-staging/              rootfs 树（chown 后为 root 属主）
├── thor-build/                  内核 O= 构建目录
└── src/                         参考项目（克隆）
    ├── diting-ubuntu/           diting 移植（补丁系列/构建系统/文档）
    ├── linux-diting/            内核树 v7.2.10（含 thor 分支 thor/v7.2.10）
    ├── fdt/                     各机型厂商 DTB 反编译（thor.dtb 是硬件参数之源）
    ├── lineage-kernel-sm8450/   LineageOS 厂商内核（面板/触摸参考）
    ├── android-mainline-port/   yzddmr6 的移植方法论手册
    └── linux-mdss-dsi-panel-driver-generator-master/  lmdpdg 面板驱动生成器
```

## 11. 构建与刷机操作手册

### 完整重建（换内核源/配置后）

```bash
cd /Users/xlisp/PyPro/xiaomi_install_ubuntu
./scripts/build-kernel-macos.sh          # Image + DTB（日志 output/kernel-build.log）
./scripts/verify-dtb.sh                  # DTB 13 项校验
./scripts/build-rootfs.sh                # staging（增量：deb 已缓存）
./scripts/build-image.sh                 # rootfs.img + boot.img + vendor_boot.img
```

### 解锁 Bootloader（一次性，人工步骤）

1. 设置 → 我的设备 → 全部参数 → 连点 OS 版本 7 次进开发者模式
2. 更多设置 → 开发者选项 → 打开「设备解锁/OEM 解锁」
3. 小米社区 App → BL 解锁 → 申请权限（等级+答题，每账号每年 1 台）
4. 开发者选项 → 设备解锁状态 → 「绑定账号和设备」→ **等 72 小时**
   （期间勿退出账号/恢复出厂/刷机）
5. `adb reboot bootloader` 后 `fastboot flashing unlock`（手机音量键确认，清空数据）

### 刷机与首启

```bash
adb reboot bootloader
./scripts/flash-thor.sh                  # 或 --boot-only 仅换内核迭代
# 首次开机 2-3 分钟（固件提取 + resize2fs），指示：
./scripts/thor-usb-net.sh up             # Mac 侧网卡 + pf NAT
./scripts/thor-usb-net.sh ssh            # ssh root@172.16.42.1
# 调试：重启回 Android 读 pstore；或 cat /sys/fs/pstore/console-ramoops-0
```

### 桌面环境（网络通了以后）

```sh
apt install phosh phoc phosh-mobile-settings   # 电话壳
# 或 gnome-shell（平板体验）；GPU/加速后续随固件提取到位
```

## 12. 当前状态与已知限制

| 阶段 | 状态 |
| --- | --- |
| 可行性调研 / 硬件逆向 | ✅ 完成 |
| 面板驱动 / 设备树 / 配置 | ✅ 完成，DTB 13 项校验通过 |
| 内核编译 | ✅ Image 61M + DTB |
| rootfs / boot 镜像 | ✅ 三件套就绪并过校验 |
| Bootloader 解锁 | ⏳ 等待小米社区 72h（唯一阻塞项） |
| 刷机 / 首启点亮 | ⬜ 待解锁后执行 |

**预期首发状态**（对标 diting bring-up 轨迹）：屏幕点亮（simplefb → 面板 DCS 初始化）、
USB NCM 网络与串口、UFS 根文件系统、触摸；WiFi/BT/音频/调制解调器随固件提取
逐步点亮；摄像头（IMX989 大底 + 多摄）是长期工作。

**已知限制**：
- 首版无 initramfs，依赖 root= 直挂（UFS/ext4 已内建）
- netfilter TCPMSS/REJECT 目标缺失（APFS 卷文件撞名，见 §6）
- PM8010 显示 PMIC 未建模（thor 独有，暂无消费者）
- rootfs 内 inode 属主为首启 chown 纠偏（构建宿主无 sudo）
- LTPO 多档刷新率仅实现 60/120 两档

## 13. 致谢与参考

- [royka1/diting-ubuntu](https://github.com/royka1/diting-ubuntu) +
  [royka1/linux-diting](https://github.com/royka1/linux-diting)——本项目的事实标准：
  SoC 级补丁、内核配置、boot 布局、固件策略、USB gadget 脚本的直接来源
- [sm8450-mainline](https://github.com/sm8450-mainline)（fdt 仓库、cupid 主线树）
- [yzddmr6/android-mainline-port](https://github.com/yzddmr6/android-mainline-port)——
  SM8475 移植方法论
- [LineageOS android_kernel_xiaomi_sm8450](https://github.com/LineageOS/android_kernel_xiaomi_sm8450)——
  厂商面板/触摸驱动参考
- [msm8916-mainline/linux-mdss-dsi-panel-driver-generator](https://github.com/msm8916-mainline/linux-mdss-dsi-panel-driver-generator)——
  厂商命令 blob 格式的权威定义
- [AOSP mkbootimg](https://android.googlesource.com/platform/system/tools/mkbootimg/)（jbeich 镜像）
- TUNA 镜像（Ubuntu ports / ubuntu-cdimage）

---
*个人实验性移植，未与 Xiaomi/Canonical/postmarketOS 有任何关联。固件不随镜像分发
（每台设备从自己的 Android 分区提取）。解锁 Bootloader 有变砖风险，操作自负。*
