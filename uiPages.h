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

// Market (trading session) definitions for timezones that host an exchange.
// Timezones without an entry here simply show no market status line.
MarketInfo getMarketInfoForTimezone(const String &tz);

// Copy the timezones saved in the project config onto the four quadrants.
// Called once during setup, before the zones are initialized over NTP.
void applyConfiguredZones();

void switchToScreen(UIScreen s);
void handleUiTouch();
void renderUiPage();

#endif // UI_PAGES_H
