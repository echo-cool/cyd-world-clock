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
    SCREEN_STATUS
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

void switchToScreen(UIScreen s);
void handleUiTouch();
void renderUiPage();

#endif // UI_PAGES_H
