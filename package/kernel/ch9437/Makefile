include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=ch9437
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define KernelPackage/ch9437
  SUBMENU:=Other modules
  TITLE:=WCH CH9437F UART expansion chip driver
  DEPENDS:=@TARGET_ramips
  FILES:=$(PKG_BUILD_DIR)/ch9437.ko
  AUTOLOAD:=$(call AutoLoad,30,ch9437,1)
  KCONFIG:=CONFIG_SERIAL_DEV_BUS
endef

define KernelPackage/ch9437/description
 WCH CH9437F 8-channel UART expansion chip driver for MT7628 UART1.
 Provides per-channel serdev controllers (kernel consumers) and
 optional ttyCHn debug ports (channels without DT child devices).
endef

MAKE_OPTS:= \
	$(KERNEL_MAKE_FLAGS) \
	SUBDIRS="$(PKG_BUILD_DIR)"

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C "$(LINUX_DIR)" \
		$(MAKE_OPTS) \
		modules
endef

$(eval $(call KernelPackage,ch9437))
