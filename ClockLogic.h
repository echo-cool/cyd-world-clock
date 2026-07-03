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

// Display objects: tft is defined in cheapYellowLCD.cpp, sprite and the
// bit-banged touch controller in ClockLogic.cpp.
extern TFT_eSPI tft;
extern TFT_eSprite sprite;
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

void rollingClockSetup(bool is24Hour, bool usDate);
void drawRollingClock();

#endif // CLOCK_LOGIC_H
