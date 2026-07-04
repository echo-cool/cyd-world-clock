#ifndef WIFI_WATCH_H
#define WIFI_WATCH_H

// ---------------------------------------------------------------------------
// Runtime WiFi supervision. The boot path already retries hard (preconfigured
// credentials -> config portal -> reboot), but a connection lost *after* boot
// used to fail silently: the fetch services just skip their work. This module
// watches the link from the main loop and reacts in three stages:
//
//  1. After WIFI_INDICATOR_AFTER_MS offline, the home faces show a steady
//     (non-blinking) "NO WIFI" label (drawn by ClockLogic.cpp).
//  2. Every few minutes offline, WiFi.reconnect() is kicked explicitly, in
//     case the WiFi stack's own auto-reconnect has wedged.
//  3. After a prolonged outage the device reboots and runs the full boot
//     recovery sequence. If the router is still down this cycles through
//     the connect retries / portal timeout until it comes back - the same
//     unattended self-healing the boot path provides after a power cut.
// ---------------------------------------------------------------------------

#include <Arduino.h>

// Offline grace period before the on-screen indicator appears.
const unsigned long WIFI_INDICATOR_AFTER_MS = 60UL * 1000UL; // 1 minute

// How long the connection has been down, in ms (0 while connected).
unsigned long wifiOfflineDurationMs();

// Outage history since boot, for the status pages: number of times the
// connection dropped, how long the last completed outage was, and how long
// ago it ended (0 when no outage has completed yet).
int wifiDropCount();
unsigned long wifiLastOutageDurationMs();
unsigned long wifiLastOutageEndedAgoMs();

// Call every loop from the main core.
void wifiWatchService();

#endif // WIFI_WATCH_H
