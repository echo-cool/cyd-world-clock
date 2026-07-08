#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

// ---------------------------------------------------------------------------
// Boot-time WiFi credential ordering.
//
// Pure logic (no Arduino / WiFi headers) so it is unit-tested on the host
// (test/test_wifi_credentials). This is the decision that the "stuck at
// System initializing..." bug lived in: boot used to try only the compiled-in
// secrets.h pair, so a network configured through the phone captive portal
// (saved in the WiFi stack's NVS) was never retried after the post-save
// reboot. orderWifiCandidates puts the portal-saved network first, with the
// built-in pair as a fallback, and drops empties / exact duplicates.
// ---------------------------------------------------------------------------

// SSID max 32 chars + NUL, WPA passphrase max 64 chars + NUL (802.11 limits).
struct WifiCandidate
{
    char ssid[33];
    char pass[65];
    const char *source; // "saved" or "built-in" (for the boot log)
};

// Fill out[] (capacity outCap, needs >= 2 for both sources) with the networks
// to try, saved before built-in, and return how many were written. A source
// with an empty SSID is skipped; the built-in pair is skipped when it is an
// exact duplicate (SSID and password) of the saved pair. Any argument may be
// null or empty. Over-long inputs are truncated to the field size.
int orderWifiCandidates(const char *savedSsid, const char *savedPass,
                        const char *builtinSsid, const char *builtinPass,
                        WifiCandidate *out, int outCap);

#endif // WIFI_CREDENTIALS_H
