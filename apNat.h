#ifndef AP_NAT_H
#define AP_NAT_H

// Shared softAP NAT + DHCP-DNS plumbing for the two captive-login helpers
// (setupPortal.cpp and wifiRelay.cpp). One copy on purpose: this code has a
// documented thread-safety footgun (see apNaptEnable) and the two users must
// stay behaviorally identical.

#include <esp_netif.h>

// The clock's softAP netif; nullptr when no AP is up.
esp_netif_t *apNetif();

// Offer the upstream (STA-learned) DNS server to softAP DHCP clients so a
// phone that renews its lease resolves names out through NAT directly.
// Without this the softAP hands out only itself as DNS and nothing answers,
// so the phone never sees the captive portal. Best-effort.
void apOfferUpstreamDns();

// Enable NAT from the AP out through the STA link; returns ESP_OK or the
// esp_netif error (ESP_ERR_INVALID_STATE when no AP netif exists). NAT must
// be toggled through esp_netif (which hops onto the lwIP task): the raw lwIP
// ip_napt_enable() trips IDF 5.x's LWIP_CHECK_THREAD_SAFETY assert when
// called from an app task and reboots the clock.
esp_err_t apNaptEnable();

// Disable NAT on the AP interface (no-op when no AP netif exists).
void apNaptDisable();

#endif // AP_NAT_H
