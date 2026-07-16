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

// The clock logic is split into modules; this header stays the umbrella so
// existing consumers keep compiling unchanged with a single include.
#include "worldZones.h"        // TradingSession/MarketInfo/WorldClockZone + worldZones[]
#include "marketStatus.h"      // market/trading-session status lines & colors
#include "brightnessControl.h" // backlight, brightness bar, LDR auto-brightness
#include "solarPhase.h"        // day/night phase, sun/moon icons, daylight bar

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

// Display objects: tft is defined in cheapYellowLCD.cpp, the bit-banged
// touch controller in ClockLogic.cpp.
extern TFT_eSPI tft;
extern XPT2046_Bitbang touchscreen;

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

// Trim text with a "..." suffix until it fits maxWidth in the current font.
String fitTextToWidth(TFT_eSPI &gfx, const String &text, int maxWidth);

// Display color for the weather-alert line and for a precipitation notice
// ("SNOW..." white, "STORM..." orange, rain cyan) - shared with the faces.
extern const uint16_t WX_ALERT_COLOR;
uint16_t weatherNoticeColor(const String &notice);

// ~14px weather-condition glyph for a WMO code centered on (cx, cy); `night`
// swaps the sun shapes for a crescent moon. Shared by the quadrant weather
// badge (ClockLogic.cpp) and the weather face rows (clockFaces.cpp).
void drawWeatherIcon(TFT_eSPI &gfx, int cx, int cy, int code, bool night);

// "HH:MM" honoring the 24-hour user setting; pm reports AM/PM for the
// indicator drawn next to the time in 12-hour mode. Shared by the quadrant
// renderer and all the alternate clock faces.
String formatHHMM(time_t local, bool &pm);

// daysFromCivil / civilFromDays now live in dateMath.h (included above).

// Touch read for all UI code: getTouch() mapped into screen pixels, mirrored
// when the display is flipped 180 degrees (projectConfig.flipDisplay) so touch
// zones always line up with what's drawn.
TouchPoint readTouchPoint();

// Synthetic touch for the /api/touch debug endpoint: readTouchPoint() reports
// a press at (x, y) - final screen pixels, matching what /screenshot shows -
// for holdMs, then a release. Downstream code (edge triggers, buttons, touch
// suppression, the alarm dismissal) sees it exactly like a finger. MAIN core
// only (the web handlers run there).
void injectTouchPoint(int x, int y, unsigned long holdMs);

void rollingClockSetup(bool is24Hour, bool usDate);
void drawRollingClock();

// Force the home screen to fully repaint on the next draw pass (all four
// zones and the static frame). clearScreen also wipes the panel immediately -
// used when arriving from another page or after an overlay painted outside
// the quadrants; pass false when the next draw pass repaints everything
// anyway.
void invalidateHomeScreen(bool clearScreen);

// Cycle to another clock face (step = 1 for next, FACE_COUNT - 1 for
// previous), persist the choice and force a full home repaint. Shared by the
// passive faces' corner taps and the timer faces' [<] / [>] buttons so the
// two can't drift apart. `source` prefixes the log line ("LOWER-LEFT touch",
// "Timer face button", ...).
void cycleClockFace(int step, const char *source);

// The quadrant sprite is a 38KB heap cache. HTTPS fetchers temporarily release
// it so mbedTLS has enough contiguous memory for certificate parsing.
bool clockReleaseRenderBufferForNetwork();
void clockRestoreRenderBufferForNetwork(bool released);

// RAII wrapper for the pair above, shared by all the fetchers: releases in
// the constructor (pass active=false to skip, e.g. a plain-HTTP request that
// doesn't need the contiguous heap) and restores in the destructor on every
// exit path.
struct RenderBufferReleaseGuard
{
    bool released;
    explicit RenderBufferReleaseGuard(bool active = true)
        : released(active ? clockReleaseRenderBufferForNetwork() : false) {}
    ~RenderBufferReleaseGuard() { clockRestoreRenderBufferForNetwork(released); }
};

#endif // CLOCK_LOGIC_H
