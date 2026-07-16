#ifndef BRIGHTNESS_CONTROL_H
#define BRIGHTNESS_CONTROL_H

// Backlight / brightness control: the manual-brightness hold, the on-screen
// brightness bar, the held-finger dim/brighten gesture and the ambient-light
// (LDR) auto-brightness with its time-of-day fallback. Moved out of
// ClockLogic.cpp/.h; the raw PWM helpers (clampBrightness / setBacklight)
// stay in brightness.h.

#include <Arduino.h>

#include "boardProfile.h" // BOARD_HAS_LDR_AUTOBRIGHTNESS

// Manual brightness override: when the user changes brightness (touch or serial),
// auto-brightness is suspended for this long so the two don't fight.
const unsigned long MANUAL_BRIGHTNESS_HOLD_MS = 2UL * 60UL * 60UL * 1000UL; // 2 hours

// How long the on-screen brightness bar stays visible after the last touch.
const unsigned long BRIGHTNESS_BAR_TIMEOUT_MS = 2000; // 2 seconds

// The night backlight level and the fallback dim window are configurable:
// projectConfig.nightBrightness / nightStartHour / nightEndHour (web
// settings page).

// --- Ambient-light (LDR) auto-brightness ------------------------------------
// The CYD has an onboard LDR on GPIO 34. Its divider circuit is unreliable on
// some board revisions (readings that never move), so auto-brightness only
// trusts the sensor after the smoothed reading has been seen to swing by at
// least LDR_MIN_SWING counts; until then - or with USE_LDR_AUTOBRIGHTNESS 0 -
// it falls back to a time-of-day schedule (the configurable night window on
// home-zone time). Use the LDR serial command to inspect the live readings.
#ifndef USE_LDR_AUTOBRIGHTNESS
#define USE_LDR_AUTOBRIGHTNESS BOARD_HAS_LDR_AUTOBRIGHTNESS
#endif
// Most CYDs read HIGH in the dark; set to 0 if yours is wired the other way
// (check with the LDR serial command: cover the sensor and watch the value).
#ifndef LDR_DARK_IS_HIGH
#define LDR_DARK_IS_HIGH 1
#endif
const int LDR_PIN = 34;        // input-only ADC pin, free on the CYD
const int LDR_MIN_SWING = 400; // counts the reading must move before trusted

extern int backlightLevel; // PWM value (0-255)

// Manual brightness hold: markManualBrightness() starts (or extends) the
// MANUAL_BRIGHTNESS_HOLD_MS suspension of auto-brightness; the queries are
// wrap-safe across the 49.7-day millis() rollover.
void markManualBrightness();
bool manualBrightnessActive();
unsigned long manualBrightnessRemainingMs(); // 0 when no hold is active

// Brightness bar state (globals so the touch UI can reset them cleanly)
extern unsigned long brightnessBarShownTime;
extern bool brightnessBarVisible;

// Paint the temporary percentage overlay used by home-screen brightness
// gestures (including the timer faces).
void showBrightnessBar(int brightness);

// One +/-1 step of a held-finger brightness gesture (self-debounced to one
// step per 10 ms). Applies the PWM, holds off auto-brightness and shows the
// bar overlay; the bar-timeout in handleTouch() persists the final level
// once the gesture ends, so this never writes flash itself.
void brightnessGestureStep(int delta);

// One auto-brightness pass (LDR when trusted, time-of-day fallback inside);
// called every home-screen loop, self-throttled to every 250 ms.
void adjustBrightnessAuto();

// Dump the ambient-light sensor state to Serial (the LDR serial command).
void printLdrStatus();

// Ambient-light sensor state for the status page / status API. Returns false
// when the LDR is compiled out (USE_LDR_AUTOBRIGHTNESS 0); otherwise fills
// whether the sensor has proven itself, the current dark/bright verdict and
// the smoothed reading.
bool getLdrState(bool &trusted, bool &dark, int &smoothed);

#endif // BRIGHTNESS_CONTROL_H
