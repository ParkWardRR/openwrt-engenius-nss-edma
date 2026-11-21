include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=leds-ipq-ledc
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/leds-ipq-ledc
  SECTION:=kernel
  CATEGORY:=Kernel modules
  SUBMENU:=LED modules
  TITLE:=Qualcomm IPQ807x LED controller (LEDC)
  DEPENDS:=@TARGET_qualcommax
  FILES:=$(PKG_BUILD_DIR)/leds-ipq-ledc.ko
  AUTOLOAD:=$(call AutoProbe,leds-ipq-ledc)
endef

define KernelPackage/leds-ipq-ledc/description
  Driver for the IPQ807x LEDC block ("qti,ledc"), which drives board LEDs over
  a serial clk/data/clr interface rather than plain GPIOs. Needed by boards
  whose LEDs hang off that block instead of gpio-leds -- the EnGenius ECW336,
  for instance, wires all five of its LEDs (power, lan, 2.4/5/6 GHz) this way,
  so without this driver such a board boots with no LED indication at all.

  Register model and the DT binding come from the QSDK driver
  (drivers/leds/leds-ipq.c); see the comment at the top of the source for what
  was changed.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,leds-ipq-ledc))
