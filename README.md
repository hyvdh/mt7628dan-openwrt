# MT7628DAN OpenWrt 固件工程

基于 OpenWrt 19.07（内核 4.14）定制的 MT7628DAN 固件工程，
硬件规格：MIPS24Kc / 64MB DDR2 / 8MB SPI Flash / 1 WAN + 4 LAN / 内置 2.4G WiFi。
自带一键编译脚本，支持**调试**与 **A/B 双分区生产**两种构建模式。

## 一键编译

```bash
./build.sh          # 调试版（默认）：调试内核 + 单分区
./build.sh ab       # AB 生产版：瘦身内核 + A/B 双分区
```

## 系统依赖

| 依赖 | 说明 |
|---|---|
| **python2**（必需） | 19.07 构建系统依赖：`sudo apt install python2 python2-dev` |
| **gcc-9**（推荐） | 主机编译器；gcc-10 与 19.07 老代码有兼容风险 |
| github SSH 重写（国内需要） | `git config --global url.git@github.com:.insteadOf https://github.com/`（mt76 等 git 源码包下载依赖，脚本启动时自动检测提示） |
| feeds | **不需要**——本设备默认包全部位于核心树 |

> 不需要 menuconfig 基础配置：两套配置种子（`mt7628dan_config/`）已就绪。

## build.sh 全部命令

| 命令 | 作用 |
|---|---|
| `./build.sh` | 调试版编译（等同 `debug`） |
| `./build.sh debug` | **调试内核 + 单分区固件**：KALLSYMS/DEBUG_FS/MAGIC_SYSRQ 开启（oops 栈可读），产物 `*mt7628dan_dev-squashfs-sysupgrade.bin` |
| `./build.sh ab` | **生产内核 + A/B 双分区固件**：内核瘦身（镜像 ≤3.4MB），产物 `*mt7628dan_dev_ab-squashfs-sysupgrade.bin` |
| `./build.sh menuconfig [debug\|ab]` | 菜单配置（默认 debug 种子；改动固化：`./scripts/diffconfig.sh > mt7628dan_config/<对应种子>`） |
| `./build.sh download` | 预下载两套配置的全部源码（可先跑一遍避免编译中途等下载） |
| `./build.sh clean` | 清理构建产物（保留工具链与 dl/ 下载缓存） |
| `./build.sh dirclean` | 深度清理（保留 dl/ 下载缓存） |

## 双构建模式说明

| | debug（调试） | ab（生产） |
|---|---|---|
| 设备名 | `mt7628dan_dev` | `mt7628dan_dev_ab` |
| 内核 | 调试版（含符号表，~1.6MB） | 瘦身版（关 KALLSYMS/eBPF 等，~1.42MB） |
| 分区 | 单 `firmware` 分区 | `firmware_a` + `firmware_b` 双槽 |
| 镜像预算 | ≤7872K | **≤3776K**（IMAGE_SIZE 守卫，超槽 B 预算编译即报错） |
| 用途 | 日常开发、问题定位 | A/B 机制验证、量产 |

## Flash 分区布局（8MB）

**单分区（debug）**：

| 偏移 | 大小 | 分区 | 说明 |
|---|---|---|---|
| 0x000000 | 192KB | u-boot | bootloader |
| 0x030000 | 64KB | u-boot-env | U-Boot 环境变量 |
| 0x040000 | 64KB | factory | **WiFi 校准数据 + MAC，勿覆盖** |
| 0x050000 | 7872KB | firmware | 内核 + squashfs + 运行时 JFFS2 overlay |

**A/B 双分区（ab）**：

| 偏移 | 大小 | 分区 | 对应 U-Boot 宏 |
|---|---|---|---|
| 0x000000 | 192KB | u-boot | — |
| 0x030000 | 64KB | u-boot-env | — |
| 0x040000 | 64KB | factory | — |
| 0x050000 | 4MB | firmware_a（槽A） | `AB_SLOT_A_ADDR=0xBC050000` |
| 0x450000 | 3776KB | firmware_b（槽B） | `AB_SLOT_B_ADDR=0xBC450000` |

两槽均自动切分 kernel / squashfs(只读) / rootfs_data(JFFS2 可写 overlay)，
镜像 ≤3.4MB 时每槽仍留有 0.4~0.8MB overlay 空间。

## 固件烧写

```bash
# 编程器：sysupgrade.bin 烧到 flash 偏移 0x50000（AB 镜像首次烧写即落入槽A）
# Web 救砖（免拆机）：设备启动时按住 WPS/RESET 进入 U-Boot HTTP 模式(192.168.1.111)
curl -F firmware=@./openwrt-*-mt7628dan_dev*-squashfs-sysupgrade.bin \
     -F press="Update firmware" 192.168.1.111 > /dev/null
```

## A/B 槽位操作（配合 U-Boot 的 abslot 模块）

U-Boot 控制台命令：

```
abslot          # 显示两槽状态（活动槽/确认标志/尝试计数）
abslot A|B      # 手动切换活动槽位
```

env 协议（U-Boot 侧自动维护，Linux 侧用 fw_setenv 配合）：

| 变量 | 含义 |
|---|---|
| `boot_slot` | 当前活动槽位 A/B |
| `slot_a_stable` / `slot_b_stable` | 槽位已确认可正常启动（1=是） |
| `slot_a_try` / `slot_b_try` | 未确认状态启动尝试次数（连续 3 次未确认自动切回另一槽） |

> 说明：老 Ralink U-Boot 的 env 存储格式与标准 fw_printenv 的兼容性需上板核实；
> 不兼容时可用 mtd 工具直接读写 env 分区（0x30000，4KB 有效数据）实现同样的协议。

## 配置种子机制

```
mt7628dan_config/
├── debug.diffconfig   # 调试版：mt7628dan_dev + xz压缩 + 调试内核选项
└── ab.diffconfig      # AB版：mt7628dan_dev_ab + xz压缩 + 内核瘦身选项
```

构建流程：`cp 种子 .config → make defconfig → make -j$(nproc)`（脚本自动完成）。
menuconfig 改动后用 `./scripts/diffconfig.sh > 种子文件` 固化。

## 工程定制清单（相对 OpenWrt 19.07 上游）

| 文件 | 内容 |
|---|---|
| `target/linux/ramips/dts/MT7628DAN-DEV.dtsi` | 设备公共定义（64MB/8MB/WiFi/MAC@factory 0x28） |
| `target/linux/ramips/dts/MT7628DAN-DEV.dts` | 单分区变体 |
| `target/linux/ramips/dts/MT7628DAN-DEV-AB.dts` | A/B 双槽变体（与 U-Boot 槽位地址对齐） |
| `target/linux/ramips/image/mt76x8.mk` | 两个 Device 定义（AB 版带 3776k 尺寸守卫） |
| `target/linux/ramips/base-files/.../02_network` | 1 WAN(P0) + 4 LAN(P1-P4) 交换配置 |
| `target/linux/ramips/config-4.14` | 内核瘦身 override（关 eBPF/JFFS2-XATTR/LZMA） |
| `config/Config-images.in` | 补上 SQUASHFS_XZ 正式 Kconfig 入口（上游遗漏） |
| `scripts/download.pl` | 下载优化：sources.cdn.openwrt.org 队首 + GNU 国内镜像(USTC/TUNA) + curl 停滞自断 |
| `build.sh` | 一键编译脚本 |
| `mt7628dan_config/` | 双模式配置种子 |

## 网络说明（国内构建机）

- 源码下载镜像已内置优化（见上表 download.pl），kernel.org/GNU/openwrt-sources 均可达
- GitHub 依赖（mt76 等 git 包）走全局 SSH 重写（见系统依赖表），需本机 SSH key 已绑定 GitHub 账号
