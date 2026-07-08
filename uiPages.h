#ifndef UI_PAGES_H
#define UI_PAGES_H

// ---------------------------------------------------------------------------
// Touch UI pages: settings, system status and timezone selection.
//
// Navigation:
//   Home screen: tap the CENTER of the screen  -> settings page
//                tap the LEFT / RIGHT third    -> brightness down / up (existing)
//   Settings:    change timezones, clock/date format, brightness, view status
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "ClockLogic.h" // WorldClockZone / MarketInfo types, display globals

enum UIScreen
{
    SCREEN_HOME,
    SCREEN_SETTINGS,
    SCREEN_ZONE_PICK,
    SCREEN_TZ_LIST,
    SCREEN_STATUS,
    SCREEN_LOGS,
    SCREEN_WIFI_LOGIN,
    SCREEN_WIFI_FAIL // portal-configured WiFi failed to join (see below)
};

extern UIScreen uiScreen;

// After a screen switch, ignore the touch panel until the finger is lifted so
// a single tap can't "click through" onto the newly drawn page.
extern bool touchSuppressedUntilRelease;

// The preset cities selectable from the timezone list (and the web settings
// page). Defined in uiPages.cpp.
struct TimezonePreset
{
    const char *name;    // label shown on the clock quadrant
    const char *tz;      // tz database name used by ezTime
    float lat, lon;      // city coordinates for the weather face (Open-Meteo)
    const char *posix;   // POSIX TZ rules, used when the timezone server is down
    const char *country; // ISO code for public holidays; "" = not covered
};

extern const TimezonePreset TZ_PRESETS[];
extern const int TZ_PRESET_COUNT;

// POSIX TZ rules for a preset timezone (nullptr for timezones outside the
// preset list). Lets a zone keep correct local time - including DST - when
// ezTime's timezone server is unreachable and nothing usable is cached.
const char *getPosixFallback(const String &tz);

// ISO country code for a preset timezone (nullptr / "" when the timezone is
// outside the preset list or its country has no public-holiday data). Used
// by the holiday service.
const char *getCountryForTimezone(const String &tz);

// Market (trading session) definitions for timezones that host an exchange.
// Timezones without an entry here simply show no market status line.
MarketInfo getMarketInfoForTimezone(const String &tz);

// Coordinates of the preset city for a timezone (used by the weather face).
// Returns false for timezones outside the preset list.
bool getCityCoords(const String &tz, float &lat, float &lon);

// Copy the timezones saved in the project config onto the four quadrants.
// Called once during setup, before the zones are initialized over NTP.
void applyConfiguredZones();

// Apply a timezone preset to a quadrant, persist it, and re-fetch the zone
// definition (brief blocking network call; falls back to the preset's POSIX
// rules if the timezone server is unreachable). Used by the touch UI and the
// web settings page - MAIN core only.
void applyZoneSelection(int slot, const TimezonePreset &preset);

// Persist the SHOW_24HOUR / NOT_US_DATE globals into the project config.
void saveDisplayPrefs();

// Human-readable last reset reason (esp_reset_reason), e.g. "power-on",
// "crash (panic)". Shown on the status pages and in /api/status.
const char *resetReasonText();

// Start the transparent captive-portal login relay and open the on-device
// helper screen. Called from the settings button and from the web /wifi-login
// "start" trigger (via the main loop). MAIN core only.
void openWifiLoginHelper();

// Boot-time UI on the "System initializing..." screen. The blocking boot
// sequence (WiFi retries -> config portal -> NTP) can take minutes on a
// network with no usable internet, so the init screen carries a Settings
// button: bootUiBegin() starts the touch panel and draws it, the boot wait
// loops call bootUiPoll() (edge -> sticky), and once it has been tapped the
// remaining network waits are cut short so the main loop starts directly on
// the settings page - from where the Wi-Fi login helper, status and logs are
// reachable. bootUiSettingsRequested() reads the sticky flag without polling.
//
// The screen doubles as a boot console: bootUiPoll() mirrors the newest log
// lines onto it, so every call also keeps the on-screen startup log moving.
// bootUiRefresh() rebuilds the whole boot screen after another page painted
// over it (the portal's conf-mode screen, SetupCYD); bootUiEnd() deactivates
// the boot UI once the main loop takes over the display.
void bootUiBegin();
void bootUiRefresh();
void bootUiEnd();
bool bootUiPoll();
bool bootUiSettingsRequested();

// Renders the WiFi-join-failure page (SCREEN_WIFI_FAIL), also reachable via
// /api/screen "wififail": records the network name and wl_status so
// the rest of boot skips its network waits (like a boot Settings tap) and the
// main loop opens SCREEN_WIFI_FAIL - a page explaining why the join failed,
// with a Reboot button and a Settings button (status / logs). The page
// reboots on its own after 5 minutes untouched (unattended recovery), and
// returns to the clock by itself if the WiFi comes up in the background.
void bootReportWifiFailure(const String &ssid, int wlStatus);

// Open whichever page a boot event asked for (WiFi-failure page wins over
// the Settings shortcut; no-op on a normal boot). Called once from setup()
// after rollingClockSetup.
void bootOpenPendingScreen();

// Remote UI driving for the /api/screen debug endpoint (otaUpdate.cpp): open
// a page by name ("home", "settings", "zones", "tzlist", "status", "logs",
// "wifilogin"); "page" picks the status / tz-list page and "slot" the tz-list
// quadrant where those apply. Returns false for an unknown name.
// uiScreenName() reports the current page for the same endpoint. Paired with
// /screenshot this lets a developer capture any UI page without touching the
// device. MAIN core only (the web handlers run there).
bool uiOpenScreenByName(const String &name, int page, int slot);
const char *uiScreenName();

void switchToScreen(UIScreen s);
void handleUiTouch();
void renderUiPage();

#endif // UI_PAGES_H
