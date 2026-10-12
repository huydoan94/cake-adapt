include $(TOPDIR)/rules.mk

PKG_LICENSE:=GPL-2.0-only
# Released together with cake-adapt.
PKG_VERSION:=0.3.17
PKG_RELEASE:=1

LUCI_TITLE:=LuCI support for cake-adapt
LUCI_DESCRIPTION:=Configure cake-adapt and watch each instance's status
LUCI_DEPENDS:=+luci-base +luci-mod-status +cake-adapt
LUCI_PKGARCH:=all

ssl ?= no
ifeq ($(ssl),yes)
  MQTT_TLS_SUPPORTED:=true
else ifeq ($(ssl),no)
  MQTT_TLS_SUPPORTED:=false
else
  $(error ssl must be yes or no)
endif
PKG_CONFIG_DEPENDS:=ssl
PKG_PREPARED_DEPENDS:=ssl

define Prepare/mqtt-tls
	$(SED) 's/^const mqttTlsSupported = false;/const mqttTlsSupported = $(MQTT_TLS_SUPPORTED);/' \
		$(PKG_BUILD_DIR)/htdocs/luci-static/resources/view/cake-adapt/config.js
endef
Hooks/Prepare/Post += Prepare/mqtt-tls

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
