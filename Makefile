include $(TOPDIR)/rules.mk

PKG_NAME:=cake-adapt
PKG_VERSION:=0.3.21
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/bpf.mk

# Three variants of one daemon, built from this Makefile: cake-adapt without
# MQTT, cake-adapt-mqtt (plain MQTT) and cake-adapt-mqtt-ssl (MQTT with TLS).
# They install the same files, so each variant provides cake-adapt and only
# one is installed; luci-app-cake-adapt's +cake-adapt takes any of them.
MQTT_SUPPORTED:=$(if $(filter mqtt mqtt-ssl,$(BUILD_VARIANT)),1,0)
MQTT_TLS_SUPPORTED:=$(if $(filter mqtt-ssl,$(BUILD_VARIANT)),1,0)
# What the installed variant supports, for LuCI: one word per line, none without MQTT.
FEATURES:=$(if $(filter 1,$(MQTT_SUPPORTED)),mqtt) $(if $(filter 1,$(MQTT_TLS_SUPPORTED)),tls)

define Package/cake-adapt/Default
  SECTION:=net
  CATEGORY:=Network
  TITLE:=CAKE monitoring and autorate daemon
  DEPENDS:=+libuci +libubox +libubus +libnl-tiny +libbpf +zlib $(BPF_DEPENDS)
endef

define Package/cake-adapt
$(call Package/cake-adapt/Default)
  VARIANT:=base
  DEFAULT_VARIANT:=1
endef

define Package/cake-adapt-mqtt
$(call Package/cake-adapt/Default)
  TITLE+= (with MQTT log forwarding)
  DEPENDS+= +libmosquitto-nossl
  VARIANT:=mqtt
  PROVIDES:=cake-adapt
endef

define Package/cake-adapt-mqtt-ssl
$(call Package/cake-adapt/Default)
  TITLE+= (with MQTT log forwarding over TLS)
  DEPENDS+= +libmosquitto-ssl +ca-bundle
  VARIANT:=mqtt-ssl
  PROVIDES:=cake-adapt
endef

define Package/cake-adapt/description
 An OpenWrt-native daemon for observing and, after verification, dynamically
 controlling CAKE bandwidth.
endef

define Package/cake-adapt-mqtt/description
$(call Package/cake-adapt/description)
 This variant can also forward its log records to an MQTT broker.
endef

define Package/cake-adapt-mqtt-ssl/description
$(call Package/cake-adapt/description)
 This variant can also forward its log records to an MQTT broker, optionally
 over TLS with certificate and hostname verification.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/Makefile ./src/*.c ./src/*/ $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR) \
		$(TARGET_CONFIGURE_OPTS) \
		CPPFLAGS="$(TARGET_CPPFLAGS)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		NETLINK_CFLAGS="-isystem $(STAGING_DIR)/usr/include/libnl-tiny" \
		NETLINK_LIBS="-lnl-tiny" \
		LIBBPF_CFLAGS="-isystem $(STAGING_DIR)/usr/include" \
		BPF_CLANG="$(CLANG)" \
		BPF_TARGET="$(BPF_TARGET)" \
		BPF_STRIP="$(LLVM_STRIP)" \
		BPF_INCLUDES="-I$(STAGING_DIR)/usr/include -I$(TOOLCHAIN_DIR)/include" \
		OBJECT_DIR="$(PKG_BUILD_DIR)/build" \
		MQTT_SUPPORTED="$(MQTT_SUPPORTED)" \
		MQTT_TLS_SUPPORTED="$(MQTT_TLS_SUPPORTED)" \
		VERSION="$(PKG_VERSION)-r$(PKG_RELEASE)"
endef

define Package/cake-adapt/conffiles
/etc/config/cake-adapt
/etc/cake-adapt/exec
endef

define Package/cake-adapt/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cake-adapt $(1)/usr/sbin/cake-adapt
	$(INSTALL_DIR) $(1)/lib/bpf
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/build/tcpdelay.bpf.o $(1)/lib/bpf/cake-adapt-tcpdelay.o
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/cake-adapt.init $(1)/etc/init.d/cake-adapt
	$(INSTALL_DIR) $(1)/usr/libexec/cake-adapt
	$(INSTALL_BIN) ./files/cake-adapt.import $(1)/usr/libexec/cake-adapt/import
	$(INSTALL_BIN) ./files/cake-adapt.import-bash $(1)/usr/libexec/cake-adapt/import-bash
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/cake-adapt.config $(1)/etc/config/cake-adapt
	$(INSTALL_DIR) $(1)/etc/cake-adapt
	$(INSTALL_CONF) ./files/cake-adapt-exec.config $(1)/etc/cake-adapt/exec
	$(INSTALL_DIR) $(1)/usr/share/cake-adapt
	$(if $(strip $(FEATURES)),printf '%s\n' $(FEATURES),printf '') > $(1)/usr/share/cake-adapt/features
endef

Package/cake-adapt-mqtt/conffiles = $(Package/cake-adapt/conffiles)
Package/cake-adapt-mqtt-ssl/conffiles = $(Package/cake-adapt/conffiles)
Package/cake-adapt-mqtt/install = $(Package/cake-adapt/install)
Package/cake-adapt-mqtt-ssl/install = $(Package/cake-adapt/install)

$(eval $(call BuildPackage,cake-adapt))
$(eval $(call BuildPackage,cake-adapt-mqtt))
$(eval $(call BuildPackage,cake-adapt-mqtt-ssl))
