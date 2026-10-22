include $(TOPDIR)/rules.mk

PKG_LICENSE:=GPL-2.0-only
# Released together with cake-adapt.
PKG_VERSION:=0.3.24
PKG_RELEASE:=1

LUCI_TITLE:=LuCI support for cake-adapt
LUCI_DESCRIPTION:=Configure cake-adapt and watch each instance's status
# Any cake-adapt variant provides cake-adapt. OpenWrt's metadata turns that
# into a select guarded by this package's own symbol and copies cake-adapt's
# @HAS_BPF_TOOLCHAIN under the same guard, which Kconfig rejects as a recursive
# dependency; naming it here, unguarded, avoids that. Any future @ dependency
# of cake-adapt (in the root Makefile) must be mirrored here the same way.
LUCI_DEPENDS:=+luci-base +luci-mod-status +cake-adapt @HAS_BPF_TOOLCHAIN
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
