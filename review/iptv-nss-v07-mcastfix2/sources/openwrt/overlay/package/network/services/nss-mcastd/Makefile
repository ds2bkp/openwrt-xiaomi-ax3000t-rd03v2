include $(TOPDIR)/rules.mk
PKG_NAME:=nss-mcastd
PKG_RELEASE:=2
PKG_LICENSE:=ISC
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)
include $(INCLUDE_DIR)/package.mk

define Package/nss-mcastd
 SECTION:=net
 CATEGORY:=Network
 TITLE:=Event-driven QCA MCS bridge membership updater
 DEPENDS:=+kmod-qca-mcs
endef

define Package/nss-mcastd/description
 Enables QCA multicast snooping on the configured IGMP downstream bridge
 and supplies listener ports to ECM when group membership changes.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -Wall -Wextra -Werror \
	-o $(PKG_BUILD_DIR)/nss-mcastd $(PKG_BUILD_DIR)/nss-mcastd.c $(TARGET_LDFLAGS)
endef

define Package/nss-mcastd/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/nss-mcastd $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/nss-mcastd.init $(1)/etc/init.d/nss-mcastd
endef
$(eval $(call BuildPackage,nss-mcastd))
