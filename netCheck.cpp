#include "netCheck.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_wifi.h>

#include "logBuffer.h"    // Log
#include "projectConfig.h" // staMacOverride

// Re-probe cadence while running (the boot probe is one-shot).
static const unsigned long NET_CHECK_EVERY_MS = 60UL * 1000UL;

// Shared state: written by netCheckNow() (boot: main core; runtime: weather
// task on core 0), read lock-free by the UI / status endpoints on the main
// core. Both are word-sized, so aligned reads/writes are atomic on the ESP32.
static volatile NetReachability g_reachability = NET_OFFLINE;
static volatile bool g_captive = false;
static unsigned long g_lastCheckMs = 0;

// Plain-HTTP connectivity canaries. HTTP (not HTTPS) on purpose: a captive
// portal has to intercept clear-text HTTP to redirect a browser, so a canary
// that returns a tiny fixed response on the open internet turns into a portal
// redirect/page when walled off. An HTTPS probe would just fail the TLS
// handshake behind a portal and tell us nothing.
struct Canary
{
    const char *url;
    int okCode;             // status code that means "real internet"
    const char *mustContain; // substring the body must hold (nullptr = ignore body)
};

static const Canary CANARIES[] = {
    // Android / Chrome: 204 No Content, empty body.
    {"http://connectivitycheck.gstatic.com/generate_204", 204, nullptr},
    // Apple: 200 with a tiny "Success" page (fallback if gstatic is blocked).
    {"http://captive.apple.com/hotspot-detect.html", 200, "Success"},
};
static const int CANARY_COUNT = sizeof(CANARIES) / sizeof(CANARIES[0]);

enum CanaryResult
{
    CANARY_OK,          // expected bare response - real internet
    CANARY_INTERCEPTED, // a portal answered with something else
    CANARY_UNREACHABLE  // DNS/connect failed - no evidence either way
};

static CanaryResult probeCanary(const Canary &c)
{
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    // A portal replies with a 302/200 pointing at its login page; we must SEE
    // that response, not chase it, so redirect-following stays off.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, c.url))
        return CANARY_UNREACHABLE;

    int code = http.GET();
    if (code <= 0)
    {
        http.end();
        return CANARY_UNREACHABLE; // DNS/connect failed
    }

    bool ok = (code == c.okCode);
    if (ok && c.mustContain != nullptr)
    {
        // Some portals answer 200 to everything, so verify the body too.
        ok = (http.getString().indexOf(c.mustContain) >= 0);
    }
    http.end();
    return ok ? CANARY_OK : CANARY_INTERCEPTED;
}

NetReachability netCheckNow()
{
    NetReachability result;
    if (WiFi.status() != WL_CONNECTED)
    {
        result = NET_OFFLINE;
    }
    else
    {
        // First canary to give a definite verdict wins; only a run where every
        // canary was unreachable falls through to OFFLINE (associated but no
        // response, and no portal evidence - so don't cry "login required").
        result = NET_OFFLINE;
        for (int i = 0; i < CANARY_COUNT; i++)
        {
            CanaryResult r = probeCanary(CANARIES[i]);
            if (r == CANARY_OK)
            {
                result = NET_ONLINE;
                break;
            }
            if (r == CANARY_INTERCEPTED)
            {
                result = NET_CAPTIVE;
                break;
            }
        }
    }

    g_reachability = result;
    bool nowCaptive = (result == NET_CAPTIVE);
    if (nowCaptive != g_captive)
    {
        g_captive = nowCaptive;
        Log.println(nowCaptive
                        ? "Captive portal detected - WiFi login required (MAC " +
                              WiFi.macAddress() + ")"
                        : "Captive portal cleared - internet reachable");
    }
    return result;
}

void netCheckService()
{
    unsigned long now = millis();
    if (g_lastCheckMs != 0 && now - g_lastCheckMs < NET_CHECK_EVERY_MS)
        return;
    g_lastCheckMs = now;
    netCheckNow();
}

NetReachability netReachability() { return g_reachability; }
bool captivePortalActive() { return g_captive; }

bool parseMac(const String &s, uint8_t out[6])
{
    unsigned int v[6];
    int n = sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6)
    {
        n = sscanf(s.c_str(), "%x-%x-%x-%x-%x-%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    }
    if (n != 6)
        return false;
    for (int i = 0; i < 6; i++)
    {
        if (v[i] > 0xFF)
            return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

String normalizeMac(const String &s)
{
    String t = s;
    t.trim();
    if (t.length() == 0)
        return "";
    uint8_t m[6];
    if (!parseMac(t, m))
        return t; // keep the typo visible so the user can fix it
    char buf[18];
    sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
            m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(buf);
}

void applyStaMacOverride()
{
    if (projectConfig.staMacOverride.length() == 0)
        return; // factory MAC

    uint8_t mac[6];
    if (!parseMac(projectConfig.staMacOverride, mac))
    {
        Log.println("Custom MAC \"" + projectConfig.staMacOverride +
                    "\" is malformed - using factory MAC");
        return;
    }
    if (mac[0] & 0x01)
    {
        // A source MAC must be unicast (even first byte); esp_wifi_set_mac
        // rejects multicast/broadcast addresses.
        Log.println("Custom MAC " + projectConfig.staMacOverride +
                    " is multicast - refused, using factory MAC");
        return;
    }

    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK)
    {
        Log.println(String("esp_wifi_set_mac failed (") + esp_err_to_name(err) +
                    ") - using factory MAC");
        return;
    }
    Log.println("Applied custom STA MAC " + projectConfig.staMacOverride +
                " (clone of an authorized device)");
}
