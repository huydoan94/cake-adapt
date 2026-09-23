include $(TOPDIR)/rules.mk

PKG_NAME:=cake-adapt
PKG_VERSION:=0.1.4
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/cake-adapt
  SECTION:=net
  CATEGORY:=Network
  TITLE:=CAKE monitoring and autorate daemon
  DEPENDS:=+libuci +libubox +libnl-tiny +zlib +fping +bash
endef

define Package/cake-adapt/description
 An OpenWrt-native daemon for observing and, after verification, dynamically
 controlling CAKE bandwidth.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/Makefile ./src/*.c ./src/*.h $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR) \
		$(TARGET_CONFIGURE_OPTS) \
		CPPFLAGS="$(TARGET_CPPFLAGS)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		NETLINK_CFLAGS="-isystem $(STAGING_DIR)/usr/include/libnl-tiny" \
		NETLINK_LIBS="-lnl-tiny" \
		OBJECT_DIR="$(PKG_BUILD_DIR)/build"
endef

define Package/cake-adapt/conffiles
/etc/config/cake-adapt
endef

define Package/cake-adapt/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/cake-adapt $(1)/usr/sbin/cake-adapt
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/cake-adapt.init $(1)/etc/init.d/cake-adapt
	$(INSTALL_DIR) $(1)/usr/libexec/cake-adapt
	$(INSTALL_BIN) ./files/cake-adapt.import $(1)/usr/libexec/cake-adapt/import
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/cake-adapt.config $(1)/etc/config/cake-adapt
endef

$(eval $(call BuildPackage,cake-adapt))
