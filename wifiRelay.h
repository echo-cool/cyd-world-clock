#ifndef WIFI_RELAY_H
#define WIFI_RELAY_H

// ---------------------------------------------------------------------------
// Transparent captive-portal login relay.
//
// Brings up a helper Wi-Fi access point while keeping the upstream STA link,
// and NATs (NAPT) the AP-side clients out through the STA interface. A phone
// that joins the helper AP and logs into the network's portal in its own
// browser thereby authorizes the CLOCK's MAC address - NAT is transparent to
// the phone's TLS/redirects, so even HTTPS/JS portals work - and when the phone
// leaves, the clock keeps the internet access. See netCheck.h for the wider
// captive-portal story and the simpler MAC-clone alternative.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <IPAddress.h>

enum RelayState
{
    RELAY_OFF,     // not running
    RELAY_ACTIVE,  // AP + NAT up, waiting for a login to grant internet
    RELAY_SUCCESS, // internet reached; AP + NAT already torn down
    RELAY_TIMEOUT  // gave up waiting; AP + NAT already torn down
};

// Bring up the helper AP + NAT (idempotent). Switches the radio to
// WIFI_AP_STA; the existing STA link is preserved. Only useful while the STA
// is associated to the (captive) upstream network.
void wifiRelayStart();

// Tear down the AP + NAT and return to STA-only. Safe to call any time.
void wifiRelayStop();

// Drive the relay: poll for internet and enforce the timeout. Call each loop
// while the helper screen is open. Flips state to RELAY_SUCCESS once online
// (and tears the AP down), or RELAY_TIMEOUT after the deadline.
void wifiRelayService();

bool wifiRelayActive();
RelayState wifiRelayState();

// Helper-AP credentials. The SSID derives from the configured hostname so two
// clocks don't collide.
String wifiRelayApSsid();
const char *wifiRelayApPassword();
IPAddress wifiRelayApIP();

// One-shot request flag: set by the web /wifi-login "start" button, consumed by
// the main UI loop so it can open the on-device helper screen. wifiRelayRequested()
// clears the flag as it reads it.
void wifiRelayRequest();
bool wifiRelayRequested();

#endif // WIFI_RELAY_H
