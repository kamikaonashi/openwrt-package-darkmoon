include $(TOPDIR)/rules.mk

PKG_NAME    := darkmoon
PKG_VERSION := 2.0.0
PKG_RELEASE := 1

PKG_LICENSE        := GPL-2.0-or-later
PKG_MAINTAINER     := kamikaonashi
PKG_BUILD_PARALLEL := 1

include $(INCLUDE_DIR)/package.mk

# $(CURDIR) is the package directory; sources live one level up.
PKG_ROOT := $(CURDIR)/..

define Package/darkmoon
  SECTION  := net
  CATEGORY := Network
  TITLE    := Adaptive CAKE bandwidth controller (cake-autorate in C)
  DEPENDS  := +libubox +libubus +libuci +kmod-sched-cake +kmod-ifb +kmod-sched-core
endef

define Package/darkmoon/description
  Native C implementation of the cake-autorate algorithm. Continuously
  measures one-way delay to a set of reflectors and adjusts the CAKE shaper
  bandwidth to keep latency low on variable-rate links (LTE/5G, Starlink,
  cable, DSL). Creates and manages its own IFB + CAKE qdiscs via netlink;
  sqm-scripts is not required.
endef

define Package/darkmoon/conffiles
/etc/config/darkmoon
endef

define Package/darkmoon/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/darkmoon enable
/etc/init.d/darkmoon restart
exit 0
endef

define Package/darkmoon/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/darkmoon stop
/etc/init.d/darkmoon disable
exit 0
endef

define Package/luci-app-darkmoon
  SECTION  := luci
  CATEGORY := LuCI
  SUBMENU  := 3. Applications
  TITLE    := LuCI interface for darkmoon
  DEPENDS  := +luci-base +darkmoon
  PKGARCH  := all
endef

define Package/luci-app-darkmoon/description
  Web interface for darkmoon: live dashboard, configuration and an
  Overview page widget.
endef

define Package/luci-app-darkmoon/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
rm -f /tmp/luci-indexcache* /tmp/luci-modulecache/* 2>/dev/null
/etc/init.d/rpcd reload
exit 0
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(PKG_ROOT)/src/*.c $(PKG_ROOT)/src/*.h $(PKG_ROOT)/src/Makefile.local \
		$(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) -f Makefile.local \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/darkmoon/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/darkmoon $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_ROOT)/root/etc/init.d/darkmoon $(1)/etc/init.d/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(PKG_ROOT)/root/etc/config/darkmoon $(1)/etc/config/
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_DATA) $(PKG_ROOT)/root/etc/uci-defaults/90-darkmoon $(1)/etc/uci-defaults/
endef

define Package/luci-app-darkmoon/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/darkmoon
	$(INSTALL_DATA) $(PKG_ROOT)/luci-app-darkmoon/www/luci-static/resources/view/darkmoon/*.js \
		$(1)/www/luci-static/resources/view/darkmoon/
	$(INSTALL_DIR) $(1)/www/luci-static/resources/darkmoon
	$(INSTALL_DATA) $(PKG_ROOT)/luci-app-darkmoon/www/luci-static/resources/darkmoon/common.js \
		$(1)/www/luci-static/resources/darkmoon/
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/status/include
	$(INSTALL_DATA) $(PKG_ROOT)/luci-app-darkmoon/www/luci-static/resources/view/status/include/75_darkmoon.js \
		$(1)/www/luci-static/resources/view/status/include/
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) $(PKG_ROOT)/luci-app-darkmoon/root/usr/share/luci/menu.d/luci-app-darkmoon.json \
		$(1)/usr/share/luci/menu.d/
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) $(PKG_ROOT)/luci-app-darkmoon/root/usr/share/rpcd/acl.d/luci-app-darkmoon.json \
		$(1)/usr/share/rpcd/acl.d/
endef

$(eval $(call BuildPackage,darkmoon))
$(eval $(call BuildPackage,luci-app-darkmoon))
