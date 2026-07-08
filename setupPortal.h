#ifndef SETUP_PORTAL_H
#define SETUP_PORTAL_H

class ProjectConfig;

// ---------------------------------------------------------------------------
// Unified AP+STA Wi-Fi setup portal (replaces the WiFiManager captive portal).
//
// One hotspot for the WHOLE flow, so the phone never has to hop between two
// different ESP32 networks:
//   1. The clock brings up a single config hotspot (AP+STA mode).
//   2. The phone joins it; a captive-portal popup opens the config page.
//   3. The user picks the target network and enters its password.
//   4. The clock connects its STA link while KEEPING the hotspot up.
//   5. If the target network needs a captive-portal login, the SAME hotspot
//      now NATs the phone's traffic out through the clock's STA link, and the
//      config page tells the user to complete the login in their phone's
//      browser. The login authorizes the clock's (STA) MAC.
//   6. As soon as the internet is reachable, the page shows "setup complete"
//      and the clock reboots into normal operation.
//
// Blocks until the clock is online (then saves config + reboots) or the portal
// times out (then reboots to retry the saved credentials). See wifiRelay.h for
// the older runtime-only relay this supersedes for first-time setup, and
// netCheck.h for the captive-portal detection it polls.
// ---------------------------------------------------------------------------
void runSetupPortal(bool forceConfig, ProjectConfig &config);

#endif // SETUP_PORTAL_H
