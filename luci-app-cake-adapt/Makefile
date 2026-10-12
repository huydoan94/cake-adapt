include $(TOPDIR)/rules.mk

PKG_LICENSE:=GPL-2.0-only
# Released together with cake-adapt.
PKG_VERSION:=0.3.17
PKG_RELEASE:=1

LUCI_TITLE:=LuCI support for cake-adapt
LUCI_DESCRIPTION:=Configure cake-adapt and watch each instance's status
LUCI_DEPENDS:=+luci-base +luci-mod-status +cake-adapt
LUCI_PKGARCH:=all

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
