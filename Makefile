include $(TOPDIR)/rules.mk

PKG_NAME:=sqm-mon
PKG_VERSION:=0.1.1
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/sqm-mon
  SECTION:=net
  CATEGORY:=Network
  TITLE:=CAKE monitoring and autorate daemon
  DEPENDS:=+libuci +libubox +libnl-tiny +zlib +fping
endef

define Package/sqm-mon/description
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

define Package/sqm-mon/conffiles
/etc/config/sqm-mon
endef

define Package/sqm-mon/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/sqm-mon $(1)/usr/sbin/sqm-mon
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/sqm-mon.init $(1)/etc/init.d/sqm-mon
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/sqm-mon.config $(1)/etc/config/sqm-mon
endef

$(eval $(call BuildPackage,sqm-mon))
