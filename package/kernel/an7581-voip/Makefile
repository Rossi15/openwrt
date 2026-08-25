include $(TOPDIR)/rules.mk

PKG_NAME:=an7581-voip
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/kernel.mk
include $(INCLUDE_DIR)/package.mk

define KernelPackage/an7581-voip
  SUBMENU:=Other modules
  TITLE:=Airoha AN7581 VoIP/ISI diagnostic driver
  DEPENDS:=@TARGET_airoha_an7581
  FILES:=$(PKG_BUILD_DIR)/an7581-voip.ko
  AUTOLOAD:=$(call AutoProbe,an7581-voip)
  KCONFIG:=
endef

define KernelPackage/an7581-voip/description
  Diagnostic driver for the AN7581 telephony PCM block and its ISI
  control interface. It only resets and identifies a connected Si32280;
  it does not enable the high-voltage converter, linefeed, or ringing.
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)" \
		modules
endef

$(eval $(call KernelPackage,an7581-voip))
