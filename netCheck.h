#ifndef NET_CHECK_H
#define NET_CHECK_H

// ---------------------------------------------------------------------------
// Internet reachability + captive-portal handling.
//
// A device can be WL_CONNECTED (associated + DHCP lease) yet still be walled
// off behind a captive portal: a login-required network blocks all traffic
// until the client authenticates on a browser page, and it grants that access
// per CLIENT MAC ADDRESS. Plain WiFi status can't tell "really online" from
// "associated but captive" apart, so NTP / weather / holidays silently fail
// and the existing wifiWatch recovery (keyed on WL_CONNECTED) never fires.
//
// This module adds:
//   - a connectivity probe that classifies online / captive / offline;
//   - a MAC-clone helper: present a user-supplied STA MAC so a network that
//     already authorized that MAC (e.g. the user's phone, or a MAC registered
//     in the network's device portal) treats the clock as authorized too.
// ---------------------------------------------------------------------------

#include <Arduino.h>

enum NetReachability
{
    NET_ONLINE,  // a connectivity canary returned its expected bare response
    NET_CAPTIVE, // associated, but a portal intercepted the canary request
    NET_OFFLINE  // not associated, or no canary answered at all
};

// Probe now (blocking - a couple of small HTTP GETs, up to a few seconds when
// nothing answers), update the shared reachability/captive state, and return
// the result. Safe on the main core at boot; at runtime it is driven from the
// weather task (core 0) via netCheckService() so it never stalls rendering.
NetReachability netCheckNow();

// Rate-limited (~60 s) wrapper around netCheckNow(). Call from the weather
// task loop; it self-gates on its own timer so calling it every tick is cheap.
void netCheckService();

// Lock-free reads of the latest classification, for the UI and /api/status.
NetReachability netReachability();
bool captivePortalActive();

// Parse "AA:BB:CC:DD:EE:FF" (or '-' separators, any case) into out[6].
// Returns false and leaves out untouched for anything malformed.
bool parseMac(const String &s, uint8_t out[6]);

// Canonical "AA:BB:CC:DD:EE:FF" when s parses; "" when s is blank; the trimmed
// input unchanged when it is non-blank but unparseable (so a typo stays
// visible in the settings field instead of silently vanishing).
String normalizeMac(const String &s);

// If projectConfig.staMacOverride holds a valid unicast MAC, push it onto the
// STA interface with esp_wifi_set_mac(). Call after WiFi.mode(WIFI_STA) and
// before WiFi.begin(). No-op when the override is empty/invalid (factory MAC).
// The custom MAC is volatile across reboots, so this must run every boot.
void applyStaMacOverride();

#endif // NET_CHECK_H
