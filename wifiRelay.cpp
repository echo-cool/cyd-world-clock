#include "wifiRelay.h"

#include <WiFi.h>
#include <esp_netif.h>
#include <lwip/lwip_napt.h>

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

// Offer the upstream DNS server to helper-AP DHCP clients so the phone can
// resolve names (and fire its own captive-portal check) with the queries NAT'd
// upstream. Without this the softAP hands out only itself as DNS and nothing
// answers, so the phone never sees the portal. Best-effort.
static void configureApDns()
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return;

    IPAddress up = WiFi.dnsIP();
    if ((uint32_t)up == 0) up = IPAddress(8, 8, 8, 8); // fallback if none learned

    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = (uint32_t)up;

    esp_netif_dhcps_stop(ap);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offerDns = 0x02; // OFFER_DNS: include the DNS option in DHCP leases
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offerDns, sizeof(offerDns));
    esp_netif_dhcps_start(ap);
}

static void teardown()
{
    // Disable NAT on the AP interface, drop the AP, and go back to STA-only.
    ip_napt_enable((uint32_t)WiFi.softAPIP(), 0);
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
    configureApDns();

    // NAT AP-side clients out through the STA interface, so their portal login
    // is attributed to the clock's (STA) MAC.
    ip_napt_enable((uint32_t)WiFi.softAPIP(), 1);

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
