#include "wifiRelay.h"

#include <WiFi.h>

#include "apNat.h"         // shared softAP NAT + DHCP-DNS plumbing
#include "logBuffer.h"     // Log
#include "netCheck.h"      // netCheckNow - success detection
#include "projectConfig.h" // hostname - helper AP SSID

// Helper-AP address + credentials. 192.168.4.1 is the ESP32 softAP default.
static const char *AP_PASSWORD = "12345678"; // matches the WiFiManager convention
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

// Give a user 15 minutes to join the AP and complete a login before giving up.
static const unsigned long RELAY_TIMEOUT_MS = 15UL * 60UL * 1000UL;
static const unsigned long RELAY_POLL_MS = 3000; // internet re-check cadence

static RelayState g_state = RELAY_OFF;
static unsigned long g_startedMs = 0;
static unsigned long g_lastPollMs = 0;
static volatile bool g_requested = false;

String wifiRelayApSsid() { return projectConfig.hostname + "-login"; }
const char *wifiRelayApPassword() { return AP_PASSWORD; }
IPAddress wifiRelayApIP() { return AP_IP; }

void wifiRelayRequest() { g_requested = true; }
bool wifiRelayRequested()
{
    bool r = g_requested;
    g_requested = false;
    return r;
}

static void teardown()
{
    // Disable NAT on the AP interface, drop the AP, and go back to STA-only.
    apNaptDisable();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
}

void wifiRelayStart()
{
    if (g_state == RELAY_ACTIVE) return;

    Log.println("Wi-Fi login helper: bringing up AP \"" + wifiRelayApSsid() +
                "\" + NAT (relaying login for STA MAC " + WiFi.macAddress() + ")");

    // Add an AP alongside the existing STA link.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
    WiFi.softAP(wifiRelayApSsid().c_str(), AP_PASSWORD);
    delay(100); // let the AP netif come up before touching DHCP / NAPT
    apOfferUpstreamDns();

    // NAT AP-side clients out through the STA interface, so their portal login
    // is attributed to the clock's (STA) MAC.
    esp_err_t naptErr = apNaptEnable();
    if (naptErr != ESP_OK)
    {
        // Keep the helper up anyway - the AP still shows the instructions -
        // but say why a login on the phone wouldn't reach the internet.
        Log.println("Wi-Fi login helper: enabling NAT failed (err " +
                    String((int)naptErr) + ") - relaying will not work");
    }

    g_state = RELAY_ACTIVE;
    g_startedMs = millis();
    g_lastPollMs = 0;
    Log.println("Wi-Fi login helper active: join \"" + wifiRelayApSsid() +
                "\" (password " + AP_PASSWORD + ") on a phone and log in");
}

void wifiRelayStop()
{
    if (g_state == RELAY_OFF) return;
    teardown();
    g_state = RELAY_OFF;
    Log.println("Wi-Fi login helper: stopped (AP + NAT down)");
}

void wifiRelayService()
{
    if (g_state != RELAY_ACTIVE) return;

    unsigned long now = millis();
    if (now - g_startedMs > RELAY_TIMEOUT_MS)
    {
        Log.println("Wi-Fi login helper: timed out with no internet");
        teardown();
        g_state = RELAY_TIMEOUT;
        return;
    }

    if (g_lastPollMs != 0 && now - g_lastPollMs < RELAY_POLL_MS) return;
    g_lastPollMs = now;

    if (netCheckNow() == NET_ONLINE)
    {
        Log.println("Wi-Fi login helper: internet reachable - login succeeded");
        teardown();
        g_state = RELAY_SUCCESS;
    }
}

bool wifiRelayActive() { return g_state == RELAY_ACTIVE; }
RelayState wifiRelayState() { return g_state; }
