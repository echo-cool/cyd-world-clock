#include "deviceIdentity.h"

#include <WiFi.h>
#include <esp_mac.h>

#include "projectConfig.h"

static bool isUsableMac(const String &mac)
{
    return mac.length() == 17 && mac != "00:00:00:00:00:00";
}

static String fallbackHardwareStaMac()
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
    {
        return "00:00:00:00:00:00";
    }
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

String deviceMacAddress()
{
    if (projectConfig.staMacOverride.length() > 0)
    {
        return projectConfig.staMacOverride;
    }

    String mac = WiFi.macAddress();
    mac.toUpperCase();
    if (isUsableMac(mac))
    {
        return mac;
    }
    return fallbackHardwareStaMac();
}

String deviceMacSuffix()
{
    String mac = deviceMacAddress();
    mac.replace(":", "");
    mac.toLowerCase();
    if (mac.length() >= 6)
    {
        return mac.substring(mac.length() - 6);
    }
    return "nomac";
}

String deviceLabel()
{
    String label = projectConfig.hostname;
    if (label.length() == 0)
    {
        label = "esp32worldclock";
    }
    return label + "-" + deviceMacSuffix();
}

String setupPortalSsid()
{
    String label = deviceLabel();
    if (label.length() <= 31)
    {
        return label;
    }

    String suffix = deviceMacSuffix();
    int prefixLen = 31 - 1 - suffix.length();
    if (prefixLen < 1)
    {
        return suffix.substring(0, min(31, (int)suffix.length()));
    }
    return label.substring(0, prefixLen) + "-" + suffix;
}
