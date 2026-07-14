NAS_MONITOR_VERSION = 1.0.0
NAS_MONITOR_SITE = $(TOPDIR)/package/nas-monitor
NAS_MONITOR_SITE_METHOD = local
NAS_MONITOR_DEPENDENCIES = paho-mqtt-c

define NAS_MONITOR_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/nas_monitor \
		$(NAS_MONITOR_SITE)/nas-monitor.c \
		-L$(STAGING_DIR)/usr/lib \
		-lpaho-mqtt3c
endef

define NAS_MONITOR_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nas_monitor $(TARGET_DIR)/usr/bin/nas-monitor
	$(INSTALL) -D -m 0644 $(NAS_MONITOR_SITE)/nas-monitor.conf $(TARGET_DIR)/etc/nas-monitor.conf
endef

$(eval $(generic-package))
