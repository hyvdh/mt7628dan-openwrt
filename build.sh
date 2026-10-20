#!/bin/bash
# MT7628DAN-DEV OpenWrt (19.07) 一键编译脚本
#
# 用法:
#   ./build.sh              默认 = 调试内核 + 单分区(日常开发调试)
#   ./build.sh debug        调试内核 + 单分区固件 mt7628dan_dev
#                           (KALLSYMS/DEBUG_FS 开启, oops 栈可读)
#   ./build.sh ab           生产内核 + A/B 双分区固件 mt7628dan_dev_ab
#                           (内核瘦身, 镜像≤3.4MB; 槽A@0x50000/槽B@0x450000
#                            与 U-Boot AB_SLOT_BOOT 地址对齐, IMAGE_SIZE=3776k
#                            守卫镜像不得超过槽B预算)
#   ./build.sh menuconfig [debug|ab]   菜单配置(默认debug种子)
#   ./build.sh download                预下载两套配置的全部源码
#   ./build.sh clean | dirclean        清理
#
# 说明:
#   - 调试版与AB版共用 board 名(custom,mt7628dan-dev), 网络配置相同
#   - 下载镜像已优化(download.pl: sources.cdn.openwrt.org 队首 + GNU 国内镜像 + 停滞自断)
#   - 主机依赖: python2(19.07必需) / gcc-9 推荐; 详见 check_env
#
set -e

TOPDIR=$(cd "$(dirname "$0")" && pwd)
cd "$TOPDIR"

CONFIG_DIR="mt7628dan_config"
SEED_DEBUG="$CONFIG_DIR/debug.diffconfig"
SEED_AB="$CONFIG_DIR/ab.diffconfig"
JOBS=$(nproc)

# ---------- 环境预检 ----------
check_env() {
	echo "== 环境检查 =="
	if ! python2 -V 2>&1 | grep -q "Python 2"; then
		echo "错误: 缺少 Python 2 (19.07 构建必需): sudo apt install python2 python2-dev"
		exit 1
	fi
	# github SSH 重写规则(mt76 等 git 源码包下载依赖, 仅提示不阻断)
	if ! git config --global --get-regexp "url\.git@github\.com:\.insteadof" >/dev/null 2>&1; then
		echo "提示: 建议配置 github SSH 重写(打通 mt76 源码下载):"
		echo "      git config --global url.git@github.com:.insteadOf https://github.com/"
	fi
}

load_config() {
	local seed="$1"
	[ -f "$seed" ] || { echo "错误: 未找到 $seed"; exit 1; }
	echo "== 加载配置: $seed =="
	cp "$seed" .config
	make defconfig
}

# ---------- 编译核心 ----------
build_with_seed() {
	local seed="$1" pattern="$2"
	check_env
	load_config "$seed"
	echo "== 开始编译 (-j$JOBS) =="
	make -j"$JOBS"
	echo
	echo "== 编译完成, 固件产物: =="
	ls -lh bin/targets/ramips/mt76x8/*${pattern}squashfs-sysupgrade.bin 2>/dev/null \
		|| echo "未找到产物, 请检查编译输出"
}

# ---------- 子命令实现区(后续在此追加) ----------
cmd_debug() {
	echo "##### 模式: 调试内核 + 单分区 #####"
	build_with_seed "$SEED_DEBUG" "mt7628dan_dev-"
}

cmd_ab() {
	echo "##### 模式: 生产内核 + A/B 双分区 #####"
	build_with_seed "$SEED_AB" "mt7628dan_dev_ab-"
}

cmd_clean() {
	echo "== make clean (清理构建产物, 保留工具链与下载缓存) =="
	make clean
}

cmd_dirclean() {
	echo "== make dirclean (深度清理, 保留 dl/ 下载缓存) =="
	make dirclean
}

cmd_menuconfig() {
	local variant="${1:-debug}"
	local seed
	case "$variant" in
		debug) seed="$SEED_DEBUG" ;;
		ab)    seed="$SEED_AB" ;;
		*)     echo "用法: $0 menuconfig [debug|ab]"; exit 1 ;;
	esac
	check_env
	load_config "$seed"
	make menuconfig
	echo "如需将改动固化回种子: ./scripts/diffconfig.sh > $seed"
}

cmd_download() {
	check_env
	for seed in "$SEED_DEBUG" "$SEED_AB"; do
		load_config "$seed"
		echo "== 预下载源码: $seed (-j$JOBS) =="
		make download -j"$JOBS"
	done
}

# ---------- 子命令分发(后续在 case 中追加新子命令) ----------
case "${1:-debug}" in
	debug)      cmd_debug ;;
	ab)         cmd_ab ;;
	clean)      cmd_clean ;;
	dirclean)   cmd_dirclean ;;
	menuconfig) shift; cmd_menuconfig "$@" ;;
	download)   cmd_download ;;
	*)
		echo "用法: $0 [子命令]"
		echo ""
		echo "  (无参数)    调试内核 + 单分区编译"
		echo "  debug       同上"
		echo "  ab          生产内核 + A/B 双分区编译"
		echo "  menuconfig [debug|ab]  菜单配置(改动可固化回种子)"
		echo "  download    预下载两套配置的全部源码"
		echo "  clean       清理构建产物(保留工具链/下载缓存)"
		echo "  dirclean    深度清理(保留下载缓存)"
		exit 1
		;;
esac
