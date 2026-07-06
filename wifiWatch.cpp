#include "wifiWatch.h"

#include <WiFi.h>

#include "logBuffer.h" // Log
#include "otaUpdate.h" // otaInProgress - never reboot mid-update
#include "wifiRelay.h" // wifiRelayActive - never reboot mid-login

// Cheap status poll cadence; WiFi.status() just reads a cached value.
static const unsigned long WIFI_CHECK_MS = 2000;

// Explicit WiFi.reconnect() kick cadence while offline. The stack's own
// auto-reconnect usually recovers on its own; this covers the cases where it
// silently gives up.
static const unsigned long WIFI_KICK_EVERY_MS = 3UL * 60UL * 1000UL; // 3 minutes

// Reboot after this long offline. A reboot re-runs the whole boot recovery
// sequence (preconfigured retries -> portal -> reboot loop), so a clock whose
// WiFi stack wedged during a multi-hour router outage rejoins on its own once
// the router is back - at the cost of the portal screen showing while the
// network is still down.
static const unsigned long WIFI_REBOOT_AFTER_MS = 30UL * 60UL * 1000UL; // 30 minutes

static bool wifiDown = false;
static unsigned long downSinceMs = 0;
static unsigned long lastKickMs = 0;
static unsigned long lastCheckMs = 0;

// Outage history since boot (status pages / /api/status)
static int dropCount = 0;
static unsigned long lastOutageDurMs = 0;
static unsigned long lastOutageEndMs = 0; // 0 = no completed outage yet

unsigned long wifiOfflineDurationMs()
{
    return wifiDown ? millis() - downSinceMs : 0;
}

int wifiDropCount()
{
    return dropCount;
}

unsigned long wifiLastOutageDurationMs()
{
    return lastOutageDurMs;
}

unsigned long wifiLastOutageEndedAgoMs()
{
    return lastOutageEndMs ? millis() - lastOutageEndMs : 0;
}

void wifiWatchService()
{
    unsigned long ms = millis();
    if (ms - lastCheckMs < WIFI_CHECK_MS)
        return;
    lastCheckMs = ms;

    if (WiFi.status() == WL_CONNECTED)
    {
        if (wifiDown)
        {
            Log.println("WiFi reconnected after " +
                        String((ms - downSinceMs) / 1000UL) + "s offline");
            wifiDown = false;
            lastOutageDurMs = ms - downSinceMs;
            lastOutageEndMs = ms;
        }
        return;
    }

    if (!wifiDown)
    {
        wifiDown = true;
        downSinceMs = ms;
        lastKickMs = ms;
        dropCount++;
        Log.println("WiFi connection lost - waiting for auto-reconnect");
        return;
    }

    // An OTA transfer owns the radio (and losing WiFi mid-update aborts it
    // anyway) - don't fight it with kicks or reboots.
    if (otaInProgress)
        return;

    // A running Wi-Fi login helper session is the user actively fixing the
    // connection - keep the reconnect kicks (the relay needs the STA link)
    // but hold the self-heal reboot until the helper closes or times out.
    if (ms - downSinceMs >= WIFI_REBOOT_AFTER_MS && !wifiRelayActive())
    {
        Log.println("WiFi offline for " + String((ms - downSinceMs) / 60000UL) +
                    " min - rebooting to run the boot recovery sequence");
        delay(200); // let the log line reach the serial port
        ESP.restart();
    }

    if (ms - lastKickMs >= WIFI_KICK_EVERY_MS)
    {
        lastKickMs = ms;
        Log.println("WiFi still offline (" + String((ms - downSinceMs) / 60000UL) +
                    " min) - kicking an explicit reconnect");
        WiFi.reconnect();
    }
}
