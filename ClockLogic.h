#ifndef CLOCK_LOGIC_H
#define CLOCK_LOGIC_H

/*-------- CYD (Cheap Yellow Display) world clock ----------*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <ezTime.h>

#include "boardProfile.h"
#include "logBuffer.h" // the Log tee used by CLOCK_DEBUG_PRINTLN and all logging
#include "dateMath.h"  // daysFromCivil / civilFromDays (host-tested pure math)

// Set to 1 to enable verbose per-frame draw/debug logging on the serial port.
// Kept at 0 for normal use so the hot draw path isn't flooded with Serial output.
#ifndef DEBUG_CLOCK
#define DEBUG_CLOCK 0
#endif
#if DEBUG_CLOCK
#define CLOCK_DEBUG_PRINTLN(x) Log.println(x)
#else
#define CLOCK_DEBUG_PRINTLN(x) do {} while (0)
#endif

const int MARKET_STATUS_MESSAGE_MIN = 10;

// Manual brightness override: when the user changes brightness (touch or serial),
// auto-brightness is suspended until manualBrightnessUntil so the two don't fight.
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

// Stock Market Information
struct TradingSession {
    int openHour;
    int openMinute;
    int closeHour;
    int closeMinute;
    String sessionName;
};

struct MarketInfo {
    String exchange;
    bool hasMarket;
    TradingSession sessions[5]; // Support up to 5 trading sessions per market
    int sessionCount;
};

// World Clock Configuration
struct WorldClockZone {
    String name;
    String timezone;
    Timezone tz;
    int lastHour;
    int lastMinute;
    bool lastIspm;
    int lastDay;
    bool initialized;
    MarketInfo market;
    String lastMarketStatus;
    String weatherAlert; // cached weather-alert text for this zone ("" = none)
    String weatherNotice; // cached rain/snow start-stop text for the date/weather row
};

// Display objects: tft is defined in cheapYellowLCD.cpp, the bit-banged
// touch controller in ClockLogic.cpp.
extern TFT_eSPI tft;
extern XPT2046_Bitbang touchscreen;

// The four clock quadrants (defined and initialized in ClockLogic.cpp)
extern WorldClockZone worldZones[4];

extern uint16_t clockBackgroundColor;
extern int screenWidth;
extern int screenHeight;
extern int quadrantWidth;
extern int quadrantHeight;

// Display format settings (mirrored to/from projectConfig by the settings UI)
extern bool SHOW_24HOUR;
extern bool NOT_US_DATE;

// Global variables for touch and backlight control
extern bool firstDraw;
extern int backlightLevel; // PWM value (0-255)
extern unsigned long manualBrightnessUntil;

// Brightness bar state (globals so the touch UI can reset them cleanly)
extern unsigned long brightnessBarShownTime;
extern bool brightnessBarVisible;

// Current market status line for a zone, e.g. "NYSE OPEN" (heavy String work -
// callers outside ClockLogic.cpp should prefer zone.lastMarketStatus, which is
// refreshed once per minute).
String getMarketStatus(WorldClockZone &zone);

// Display color for a market status string ("NYSE OPEN" -> green, ...).
uint16_t getMarketStatusColor(String status);

// Day/night-dependent colors for a zone's local time (used for time digits
// and for labels/dates respectively). Preset cities carry coordinates, so
// day/night follows the sun's real position (sunrise/sunset incl. seasons);
// zones without known coordinates fall back to fixed 6AM-6PM windows.
uint16_t getDayNightColor(WorldClockZone &zone);
uint16_t getDayNightLabelColor(WorldClockZone &zone);

// "HH:MM" honoring the 24-hour user setting; pm reports AM/PM for the
// indicator drawn next to the time in 12-hour mode. Shared by the quadrant
// renderer and all the alternate clock faces.
String formatHHMM(time_t local, bool &pm);

// daysFromCivil / civilFromDays now live in dateMath.h (included above).

// Touch read for all UI code: getTouch() mapped into screen pixels, mirrored
// when the display is flipped 180 degrees (projectConfig.flipDisplay) so touch
// zones always line up with what's drawn.
TouchPoint readTouchPoint();

// Dump the ambient-light sensor state to Serial (the LDR serial command).
void printLdrStatus();

// Ambient-light sensor state for the status page / status API. Returns false
// when the LDR is compiled out (USE_LDR_AUTOBRIGHTNESS 0); otherwise fills
// whether the sensor has proven itself, the current dark/bright verdict and
// the smoothed reading.
bool getLdrState(bool &trusted, bool &dark, int &smoothed);

void rollingClockSetup(bool is24Hour, bool usDate);
void drawRollingClock();

// The quadrant sprite is a 38KB heap cache. HTTPS fetchers temporarily release
// it so mbedTLS has enough contiguous memory for certificate parsing.
bool clockReleaseRenderBufferForNetwork();
void clockRestoreRenderBufferForNetwork(bool released);

#endif // CLOCK_LOGIC_H
