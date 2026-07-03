#ifndef CLOCK_LOGIC_H
#define CLOCK_LOGIC_H

/*-------- CYD (Cheap Yellow Display) world clock ----------*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>
#include <ezTime.h>

// Set to 1 to enable verbose per-frame draw/debug logging on the serial port.
// Kept at 0 for normal use so the hot draw path isn't flooded with Serial output.
#ifndef DEBUG_CLOCK
#define DEBUG_CLOCK 0
#endif
#if DEBUG_CLOCK
#define CLOCK_DEBUG_PRINTLN(x) Serial.println(x)
#else
#define CLOCK_DEBUG_PRINTLN(x) do {} while (0)
#endif

const int MARKET_STATUS_MESSAGE_MIN = 10;

// Manual brightness override: when the user changes brightness (touch or serial),
// auto-brightness is suspended until manualBrightnessUntil so the two don't fight.
const unsigned long MANUAL_BRIGHTNESS_HOLD_MS = 2UL * 60UL * 60UL * 1000UL; // 2 hours

// How long the on-screen brightness bar stays visible after the last touch.
const unsigned long BRIGHTNESS_BAR_TIMEOUT_MS = 2000; // 2 seconds

// Backlight level used in a dark room / at night by auto-brightness.
const int NIGHT_BRIGHTNESS = 1;

// --- Ambient-light (LDR) auto-brightness ------------------------------------
// The CYD has an onboard LDR on GPIO 34. Its divider circuit is unreliable on
// some board revisions (readings that never move), so auto-brightness only
// trusts the sensor after the smoothed reading has been seen to swing by at
// least LDR_MIN_SWING counts; until then - or with USE_LDR_AUTOBRIGHTNESS 0 -
// it falls back to the time-of-day schedule (dim 1-7 AM home time). Use the
// LDR serial command to inspect the live readings.
#ifndef USE_LDR_AUTOBRIGHTNESS
#define USE_LDR_AUTOBRIGHTNESS 1
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
};

// Display objects: tft is defined in cheapYellowLCD.cpp, the bit-banged
// touch controller in ClockLogic.cpp.
extern TFT_eSPI tft;
extern XPT2046_Bitbang touchscreen;

// The four clock quadrants (defined and initialized in ClockLogic.cpp)
extern WorldClockZone worldZones[4];

extern uint16_t clockBackgroundColor;

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
// and for labels/dates respectively).
uint16_t getDayNightColor(Timezone &tz);
uint16_t getDayNightLabelColor(Timezone &tz);

// "HH:MM" honoring the 24-hour user setting; pm reports AM/PM for the
// indicator drawn next to the time in 12-hour mode. Shared by the quadrant
// renderer and all the alternate clock faces.
String formatHHMM(time_t local, bool &pm);

// Days since 1970-01-01 for a civil date; ((result + 4) % 7) gives the day of
// week with 0 = Sunday.
long daysFromCivil(int y, int m, int d);

// Inverse of daysFromCivil: civil date for a days-since-1970 count. Used to
// walk forward over real calendar dates (weekends, market holidays).
void civilFromDays(long days, int &y, int &m, int &d);

// Dump the ambient-light sensor state to Serial (the LDR serial command).
void printLdrStatus();

void rollingClockSetup(bool is24Hour, bool usDate);
void drawRollingClock();

#endif // CLOCK_LOGIC_H
