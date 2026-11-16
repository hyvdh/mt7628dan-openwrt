include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=ch9437-dev
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define KernelPackage/ch9437-dev
  SUBMENU:=Other modules
  TITLE:=CH9437 sub-uart generic transport device driver
  DEPENDS:=+kmod-ch9437
  FILES:=$(PKG_BUILD_DIR)/ch9437-dev.ko
  AUTOLOAD:=$(call AutoLoad,31,ch9437-dev,1)
endef

define KernelPackage/ch9437-dev/description
 Generic serdev transport driver for devices attached to CH9437
 sub-uarts. Exposes one character device node per device
 (/dev/<dt-node-name>) with raw read/write/poll and transport
 level ioctl. Protocol parsing stays in userspace.
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

$(eval $(call KernelPackage,ch9437-dev))
