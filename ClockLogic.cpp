/*-------- CYD (Cheap Yellow Display) ----------*/

#include "ClockLogic.h"

#include <WiFi.h> // link status for the zone-setup boot log

#include "brightness.h"
#include "clockFaces.h"
#include "genericBaseProject.h" // BACKLIGHT_PIN
#include "holidayService.h"     // holidaysBegin, getHolidayName
#include "marketHolidays.h"
#include "fontTimeDigits.h" // anti-aliased VLW digits for the quadrant times
#include "projectConfig.h"  // home-screen extras toggles
#include "serialCommands.h"
#include "uiPages.h"
#include "weatherService.h" // weatherBegin
#include "logShipper.h"     // logShipperBegin - remote log push
#include "wifiWatch.h"      // offline indicator on the home faces
#include "netCheck.h"       // captivePortalActive - login-required indicator
#include "wifiRelay.h"      // wifiRelayRequested - web-triggered login helper
#include "timeFormat.h"     // puretime::formatHHMM - host-tested 12/24h formatting
#include "marketSession.h"  // host-tested trading-session membership math

// Off-screen buffer for one full clock quadrant (160x120x16bpp = 38KB). Each
// quadrant is rendered here and pushed to the panel in a single blit, so the
// per-minute updates never show a half-drawn (black-flashing) quadrant. If the
// allocation ever fails the code falls back to drawing directly on the panel.
static TFT_eSprite quadSprite = TFT_eSprite(&tft);
static bool quadSpriteOk = false;
static bool quadSpriteWanted = false;
static SemaphoreHandle_t quadSpriteMutex = nullptr;

static void ensureQuadSpriteMutex()
{
    if (!quadSpriteMutex) quadSpriteMutex = xSemaphoreCreateMutex();
}

// Touch screen pins (bit-banged SPI)
#define MOSI_PIN 32
#define MISO_PIN 39
#define CLK_PIN  25
#define CS_PIN   33

XPT2046_Bitbang touchscreen(MOSI_PIN, MISO_PIN, CLK_PIN, CS_PIN);

int clockFont = 4;  // Changed to font 4 for better readability
int clockSize = 2;  // Moderate size for Font 4 to fit in quadrants
int clockDatum = TL_DATUM;
uint16_t clockBackgroundColor = TFT_BLACK;
uint16_t clockFontColor = TFT_WHITE;  // Changed to white for better contrast

bool SHOW_24HOUR = true;

bool NOT_US_DATE = true;

WorldClockZone worldZones[4] = {
    {"SANTA CLARA", "America/Los_Angeles", Timezone(), -1, -1, false, -1, false,
     {"", false, {}, 0}, ""},
    {"NEW YORK", "America/New_York", Timezone(), -1, -1, false, -1, false,
     {"NYSE", true, {
         {9, 30, 16, 0, "REGULAR"},      // Regular trading Mon-Fri
         {16, 0, 20, 0, "AFTER-HRS"},    // After-hours trading Mon-Fri
         {20, 0, 4, 0, "OVERNIGHT"},     // Overnight trading (spans midnight, Fri-Sun)
         {4, 0, 9, 30, "PRE-MARKET"}     // Pre-market trading (Sun 6PM = Mon 4AM equiv)
     }, 4}, ""},
    {"BEIJING", "Asia/Shanghai", Timezone(), -1, -1, false, -1, false,
     {"SSE", true, {
         {9, 0, 9, 30, "PRE-MARKET"},     // PRE-MARKET session
         {9, 30, 11, 30, "REGULAR"},     // Morning session
         {13, 0, 15, 0, "REGULAR"},    // Afternoon session
         {15, 0, 15, 30, "AFTER-HRS"}   // After-hours session
     }, 4}, ""},
    {"HONG KONG", "Asia/Hong_Kong", Timezone(), -1, -1, false, -1, false,
     {"HKEX", true, {
         {9, 30, 12, 0, "REGULAR"},      // Morning session
         {13, 0, 16, 0, "REGULAR"}       // Afternoon session
     }, 2}, ""}
};

// Global variables for touch and backlight control
bool firstDraw = true;
int backlightLevel = 80; // PWM value (0-255)

// Brightness bar state (globals so the touch UI can reset them cleanly)
unsigned long brightnessBarShownTime = 0;
bool brightnessBarVisible = false;

// Manual brightness override: when the user changes brightness (touch or serial),
// auto-brightness is suspended until this timestamp so the two don't fight.
unsigned long manualBrightnessUntil = 0;

// Global variables for flashing market status messages
unsigned long lastFlashTime = 0;
bool flashState = true;
const unsigned long flashInterval = 1000; // 1 second
bool flashJustChanged = false;

// Weather-alert alternation on the quadrant day line: a zone with an alert
// slowly swaps the weekday/holiday line with the alert text. The normal day
// slot is longer so the clock's date context stays the primary read; the swap
// is slow (seconds) rather than a blink, to avoid distraction.
const unsigned long WX_ALERT_DAY_MS = 6000;    // weekday/holiday shown per cycle
const unsigned long WX_ALERT_ALERT_MS = 4000;  // weather alert shown per cycle
const uint16_t WX_ALERT_COLOR = TFT_ORANGE;    // distinct from any market color
static bool wxAlertShowingAlert = false;       // false = day slot, true = alert
static bool wxAlertPhaseJustChanged = false;
static unsigned long wxAlertPhaseSince = 0;

// Function declarations for flashing functionality
bool shouldMessageFlash(String message);
void updateFlashState();
void updateWeatherAlertPhase();
void resetFlashChangeFlag();
void updateDynamicQuadrantArea(WorldClockZone &zone, int quadrantIndex);
bool needsDynamicQuadrantAreaUpdate(WorldClockZone &zone);
void adjustBrightnessAuto();

void SetupCYD()
{
    Log.println("SetupCYD");
    // tft.init();
    tft.fillScreen(clockBackgroundColor);
    tft.setTextColor(clockFontColor, clockBackgroundColor);

    tft.setRotation(projectConfig.flipDisplay ? 3 : 1);
    tft.setTextFont(clockFont);
    tft.setTextSize(clockSize);
    tft.setTextDatum(clockDatum);
}

// Screen dimensions (320x240)
int screenWidth = 320;
int screenHeight = 240;
int quadrantWidth = 160;  // 320/2
int quadrantHeight = 120; // 240/2

static bool createQuadSpriteLocked()
{
    if (quadSpriteOk) return true;
    quadSpriteOk = (quadSprite.createSprite(quadrantWidth, quadrantHeight) != nullptr);
    return quadSpriteOk;
}

// Quadrant positions for 4 timezones
struct QuadrantPos {
    int x, y, centerX, centerY;
};

QuadrantPos quadrants[4] = {
    {0, 0, 80, 60},           // Top-left
    {160, 0, 240, 60},        // Top-right
    {0, 120, 80, 180},        // Bottom-left
    {160, 120, 240, 180}      // Bottom-right
};

String formatHHMM(time_t local, bool &pm)
{
    // The 12/24-hour conversion is unit-tested in puretime::formatHHMM; here we
    // just feed it ezTime's already-extracted hour/minute.
    return String(puretime::formatHHMM(hour(local), minute(local), SHOW_24HOUR, pm).c_str());
}

// Sun elevation in degrees above the horizon at utc for a site, via NOAA's
// simplified solar position algorithm (Fourier-series equation of time and
// declination). Accurate to ~0.1 degrees - plenty for picking display colors.
static float solarElevationDeg(float latDeg, float lonDeg, time_t utc)
{
    int doy = (int)(daysFromCivil(year(utc), month(utc), day(utc)) -
                    daysFromCivil(year(utc), 1, 1)) + 1;
    float hourUtc = hour(utc) + minute(utc) / 60.0f;
    float g = 2.0f * PI / 365.0f * (doy - 1 + (hourUtc - 12.0f) / 24.0f);
    float eqtimeMin = 229.18f * (0.000075f + 0.001868f * cosf(g) - 0.032077f * sinf(g)
                                 - 0.014615f * cosf(2 * g) - 0.040849f * sinf(2 * g));
    float declRad = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g)
                    - 0.006758f * cosf(2 * g) + 0.000907f * sinf(2 * g)
                    - 0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
    float trueSolarMin = hourUtc * 60.0f + eqtimeMin + 4.0f * lonDeg;
    float hourAngleDeg = trueSolarMin / 4.0f - 180.0f;
    while (hourAngleDeg < -180.0f) hourAngleDeg += 360.0f;
    while (hourAngleDeg > 180.0f) hourAngleDeg -= 360.0f;
    float latRad = latDeg * DEG_TO_RAD;
    float cosZen = sinf(latRad) * sinf(declRad) +
                   cosf(latRad) * cosf(declRad) * cosf(hourAngleDeg * DEG_TO_RAD);
    if (cosZen > 1.0f) cosZen = 1.0f;
    if (cosZen < -1.0f) cosZen = -1.0f;
    return 90.0f - acosf(cosZen) * RAD_TO_DEG;
}

enum DayPhase
{
    PHASE_DAY,     // sun above the horizon
    PHASE_EVENING, // sun down, before local midnight
    PHASE_NIGHT    // sun down, small hours
};

static DayPhase zoneDayPhase(WorldClockZone &zone)
{
    time_t local = zone.tz.now();
    int hr = hour(local);

    float lat, lon;
    if (getCityCoords(zone.timezone, lat, lon)) {
        // True day/night from the sun's actual position (so London reads as
        // night at 4:30 PM in December and as day at 9 PM in June). -0.833
        // degrees is the standard sunrise/sunset threshold: the sun's upper
        // limb on the horizon, corrected for atmospheric refraction.
        if (solarElevationDeg(lat, lon, UTC.now()) > -0.833f) {
            return PHASE_DAY;
        }
        return hr >= 12 ? PHASE_EVENING : PHASE_NIGHT;
    }

    // Zone outside the preset list (no coordinates): fixed windows, as before
    if (hr >= 6 && hr < 18) return PHASE_DAY;
    return hr >= 18 ? PHASE_EVENING : PHASE_NIGHT;
}

// Cool "night sky" blues used for evening/night text when the readable
// night-colors option (projectConfig.dayNightIcons) is on. The legacy greys
// encoded the phase in brightness, but dark grey digits on the black
// background - on top of the auto-dimmed backlight - made the time hardest
// to read exactly when it's glanced at half-awake. With the option on, the
// sun/moon icon carries the day/night meaning instead, so the text can stay
// readable: warm by day, cool blue at night.
static const uint16_t EVENING_TEXT_COLOR = 0x965F; // light ice blue
static const uint16_t NIGHT_TEXT_COLOR = 0x7D5D;   // dimmer steel blue

uint16_t getDayNightColor(WorldClockZone &zone)
{
    bool readable = projectConfig.dayNightIcons;
    switch (zoneDayPhase(zone)) {
    case PHASE_DAY: return TFT_ORANGE;
    case PHASE_EVENING: return readable ? EVENING_TEXT_COLOR : TFT_LIGHTGREY;
    default: return readable ? NIGHT_TEXT_COLOR : TFT_DARKGREY;
    }
}

uint16_t getDayNightLabelColor(WorldClockZone &zone)
{
    bool readable = projectConfig.dayNightIcons;
    switch (zoneDayPhase(zone)) {
    case PHASE_DAY: return TFT_YELLOW;
    case PHASE_EVENING: return readable ? EVENING_TEXT_COLOR : TFT_LIGHTGREY;
    default: return readable ? NIGHT_TEXT_COLOR : TFT_DARKGREY;
    }
}

// ~12px sun (day) or crescent moon (evening/night) so the day/night state
// doesn't ride on text color alone. Sized/positioned to clear the longest
// centered city names (e.g. SANTA CLARA starts at quadrant x=14).
static void drawDayNightIcon(TFT_eSPI &gfx, int cx, int cy, DayPhase phase)
{
    if (phase == PHASE_DAY) {
        gfx.fillCircle(cx, cy, 3, TFT_YELLOW);
        static const int8_t rays[8][4] = {
            {5, 0, 6, 0}, {-5, 0, -6, 0}, {0, 5, 0, 6}, {0, -5, 0, -6},
            {4, 4, 5, 5}, {-4, 4, -5, 5}, {4, -4, 5, -5}, {-4, -4, -5, -5}};
        for (int i = 0; i < 8; i++) {
            gfx.drawLine(cx + rays[i][0], cy + rays[i][1],
                         cx + rays[i][2], cy + rays[i][3], TFT_YELLOW);
        }
    } else {
        uint16_t c = (phase == PHASE_EVENING) ? EVENING_TEXT_COLOR : NIGHT_TEXT_COLOR;
        gfx.fillCircle(cx, cy, 4, c);
        gfx.fillCircle(cx + 3, cy - 1, 4, clockBackgroundColor); // carve the crescent
    }
}


// Days since 1970-01-01 for a given civil date (Howard Hinnant's algorithm).
// Used to compare calendar dates between timezones robustly across month/year
// boundaries instead of comparing bare day-of-month numbers.
// Show the countdown to the next open only when the open is less than a day
// away. Longer waits used to render as "OPENS IN 2D 8H", which reads too much
// like a time ("20 8H") at a glance - those now show a plain red "CLOSED".
static const long MARKET_COUNTDOWN_MAX_MIN = 24 * 60;

// Format a sub-24h countdown as "5H 03M" or "45 MIN" depending on magnitude.
static String formatOpenCountdown(long minutes)
{
    char buf[12];
    if (minutes >= 60) {
        sprintf(buf, "%ldH %02ldM", minutes / 60, minutes % 60);
    } else {
        sprintf(buf, "%ld MIN", minutes);
    }
    return String(buf);
}

// Minutes until the next REGULAR session opens, walking real calendar dates
// so weekends AND full-day exchange holidays (marketHolidays.cpp) are skipped.
// The scan covers 30 days so long closures (SSE Spring Festival / golden
// week) still resolve. DST shifts between now and the open are ignored (at
// most an hour off around a transition weekend); half-day early closes don't
// move the opens, so they don't matter here.
static long minutesToNextRegularOpen(const WorldClockZone &zone, int y, int mo, int dd, int totalMinutes)
{
    long today = daysFromCivil(y, mo, dd);
    for (int d = 0; d <= 30; d++) {
        long days = today + d;
        int wd = (int)((days + 4) % 7) + 1; // 1=Sunday ... 7=Saturday
        if (wd == 1 || wd == 7) continue;   // markets closed on weekends
        int fy, fm, fd;
        civilFromDays(days, fy, fm, fd);
        if (isMarketHoliday(zone.market.exchange, fy, fm, fd)) continue;
        long best = -1;
        for (int i = 0; i < zone.market.sessionCount; i++) {
            const TradingSession &session = zone.market.sessions[i];
            if (session.sessionName != "REGULAR") continue;
            int start = session.openHour * 60 + session.openMinute;
            if (d == 0 && start <= totalMinutes) continue; // already past today
            long m = d * 1440L + start - totalMinutes;
            if (best < 0 || m < best) best = m;
        }
        if (best >= 0) return best;
    }
    return -1;
}

// Status line for a market that is closed: when the next regular open is
// under 24 hours away, count down to it; further out (long holidays,
// weekends viewed early) show a plain "CLOSED". Worded "OPENS IN" (not
// "OPEN IN") so shouldMessageFlash() doesn't treat this always-on countdown
// as one of the <=10 minute flashing alerts.
static String closedStatusWithCountdown(const WorldClockZone &zone, int y, int mo, int dd, int totalMinutes)
{
    long m = minutesToNextRegularOpen(zone, y, mo, dd, totalMinutes);
    if (m < 0 || m >= MARKET_COUNTDOWN_MAX_MIN) return zone.market.exchange + " CLOSED";
    return zone.market.exchange + " OPENS IN " + formatOpenCountdown(m);
}

String getMarketStatus(WorldClockZone &zone)
{
    if (!zone.market.hasMarket) {
        return ""; // No market for this zone
    }

    time_t local = zone.tz.now();
    int currentHour = hour(local);
    int currentMinute = minute(local);
    int currentDayOfWeek = weekday(local); // 1=Sunday, 2=Monday, ..., 7=Saturday
    int currentTotalMinutes = currentHour * 60 + currentMinute;
    int currentYear = year(local);
    int currentMonth = month(local);
    int currentDayOfMonth = day(local);

    // Full-day exchange holiday: closed all day, countdown to the next open
    // (the countdown itself skips holidays too).
    if (isMarketHoliday(zone.market.exchange, currentYear, currentMonth, currentDayOfMonth)) {
        return closedStatusWithCountdown(zone, currentYear, currentMonth, currentDayOfMonth, currentTotalMinutes);
    }

    // Half-day early close (e.g. NYSE Black Friday 1 PM): the session loop
    // below truncates sessions at this time and skips the ones that would
    // start after it. -1 on a normal day.
    int earlyCloseMinutes = marketEarlyCloseMinutes(zone.market.exchange, currentYear,
                                                    currentMonth, currentDayOfMonth);

    // Weekend logic - NYSE is closed on Saturday and Sunday
    if (currentDayOfWeek == 7 || currentDayOfWeek == 1) { // Saturday or Sunday
        // Check for Sunday evening futures/overnight trading starting at 6 PM ET
        if (currentDayOfWeek == 1 && zone.market.exchange == "NYSE" && currentTotalMinutes >= 18 * 60) { // Sunday after 6 PM
            // Check if we have overnight sessions that start Sunday evening
            for (int i = 0; i < zone.market.sessionCount; i++) {
                TradingSession session = zone.market.sessions[i];
                if (session.sessionName == "OVERNIGHT") {
                    // Sunday 6 PM ET start time for futures markets
                    int sundayStart = 18 * 60; // 6 PM Sunday
                    int mondayEnd = session.closeHour * 60 + session.closeMinute; // Monday 4 AM

                    if (currentTotalMinutes >= sundayStart) {
                        // Calculate minutes until Monday 4 AM close
                        int minutesToClose = (24 * 60) - currentTotalMinutes + mondayEnd;
                        if (minutesToClose <= MARKET_STATUS_MESSAGE_MIN) {
                            return zone.market.exchange + " " + session.sessionName + " CLOSE IN " + String(minutesToClose) + " MIN";
                        }
                        return zone.market.exchange + " " + session.sessionName + " OPEN";
                    }
                }
            }
        }
        return closedStatusWithCountdown(zone, currentYear, currentMonth, currentDayOfMonth, currentTotalMinutes);
    } else if (currentDayOfWeek == 6) { // Friday
        // Friday may have extended evening hours - check if overnight sessions
        // extend into the weekend. Not on a half day (e.g. the day after
        // Thanksgiving is always a Friday): the evening sessions don't run.
        if (zone.market.exchange == "NYSE" && earlyCloseMinutes < 0) {
            for (int i = 0; i < zone.market.sessionCount; i++) {
                TradingSession session = zone.market.sessions[i];
                if (session.sessionName == "OVERNIGHT" && currentTotalMinutes >= 20 * 60) { // After 8 PM Friday
                    // Overnight session continues into weekend (Friday 8 PM to Sunday)
                    return zone.market.exchange + " OVERNIGHT OPEN";
                }
            }
        }
        // Otherwise check normal sessions below
    }

    // Check each trading session
    for (int i = 0; i < zone.market.sessionCount; i++) {
        TradingSession session = zone.market.sessions[i];
        if (session.sessionName.length() == 0) continue; // Skip empty sessions

        int sessionStart = session.openHour * 60 + session.openMinute;
        int sessionEnd = session.closeHour * 60 + session.closeMinute;

        // On a half day the exchange stops at earlyCloseMinutes: sessions
        // that would start at/after it don't run, the one in progress ends
        // there instead.
        if (earlyCloseMinutes >= 0) {
            if (sessionEnd < sessionStart) {
                // Spans midnight: tonight's session won't start. Keep only the
                // after-midnight tail, which belongs to the previous
                // (full-length) trading day.
                if (currentTotalMinutes >= sessionEnd) continue;
            } else {
                if (sessionStart >= earlyCloseMinutes) continue;
                if (sessionEnd > earlyCloseMinutes) sessionEnd = earlyCloseMinutes;
            }
        }

        // Membership + countdown math (incl. the midnight-spanning cases)
        // lives in market::* so it is unit-tested on the host.
        if (market::inSession(currentTotalMinutes, sessionStart, sessionEnd)) {
            // Currently in this session - check if closing soon
            int minutesToClose = market::minutesUntilClose(currentTotalMinutes, sessionStart, sessionEnd);

            if (minutesToClose <= MARKET_STATUS_MESSAGE_MIN) {
                if (session.sessionName == "REGULAR") {
                    return zone.market.exchange + " CLOSE IN " + String(minutesToClose) + " MIN";
                } else {
                    return zone.market.exchange + " " + session.sessionName + " CLOSE IN " + String(minutesToClose) + " MIN";
                }
            }

            if (session.sessionName == "REGULAR") {
                return zone.market.exchange + " OPEN";
            } else {
                return zone.market.exchange + " " + session.sessionName + " OPEN";
            }
        }

        // Check if next session is opening soon. A negative result means a
        // normal same-day session whose open is already past - look to the
        // next day (handled by later sessions / the closed-countdown below).
        int minutesToOpen = market::minutesUntilOpen(currentTotalMinutes, sessionStart, sessionEnd);
        if (minutesToOpen < 0) {
            continue;
        }

        if (minutesToOpen <= MARKET_STATUS_MESSAGE_MIN) {
            if (session.sessionName == "REGULAR") {
                return zone.market.exchange + " OPEN IN " + String(minutesToOpen) + " MIN";
            } else {
                return zone.market.exchange + " " + session.sessionName + " OPEN IN " + String(minutesToOpen) + " MIN";
            }
        }
    }

    return closedStatusWithCountdown(zone, currentYear, currentMonth, currentDayOfMonth, currentTotalMinutes);
}

uint16_t getMarketStatusColor(String status)
{
    // Countdown to the next regular open ("NYSE OPENS IN 5H 03M") - checked
    // first because it would otherwise match the generic " OPEN" case below.
    // Yellow: "about to open" sits between closed (red) and open (green).
    if (status.indexOf("OPENS IN") != -1) {
        return TFT_YELLOW;
    }

    // Check for specific session types first (before generic OPEN check)
    if (status.indexOf("AFTER-HRS OPEN") != -1) {
        return TFT_CYAN;    // Cyan for extended/after-hours trading
    } else if (status.indexOf("OVERNIGHT OPEN") != -1 || status.indexOf("PRE-MARKET OPEN") != -1) {
        return TFT_BLUE;    // Blue for overnight/pre-market
    } else if (status.indexOf("CLOSING OPEN") != -1) {
        return TFT_YELLOW;  // Yellow for closing auction period
    } else if (status.indexOf("CLOSED") != -1) {
        return TFT_RED;     // Red for closed market
    } else if (status.indexOf(" OPEN") != -1 && status.indexOf("OPEN ") == -1) {
        return TFT_GREEN;   // Bright green for regular trading (e.g. "NYSE OPEN")
    } else if (status.indexOf("OPEN ") != -1) {
        return TFT_YELLOW;  // Yellow for opening soon countdown (e.g. "NYSE OPEN 15MIN")
    } else if (status.indexOf("CLOSE ") != -1) {
        return TFT_ORANGE;  // Orange for closing soon countdown (e.g. "NYSE CLOSE 10MIN")
    } else {
        return TFT_WHITE;   // Default color
    }
}

// Color for the daylight bar at a given solar elevation: deep blue night,
// dark blue twilight, orange around the horizon, warm yellow midday.
// Piecewise-linear blend between the anchor stops.
static uint16_t daylightBarColor(float elevDeg)
{
    static const struct { float e; uint8_t r, g, b; } stops[] = {
        {-18.0f, 8, 10, 35},    // astronomical night
        {-6.0f, 30, 40, 90},    // civil twilight
        {0.0f, 200, 90, 25},    // sunrise / sunset
        {12.0f, 255, 160, 30},  // low sun
        {40.0f, 255, 225, 90},  // high sun
    };
    const int n = sizeof(stops) / sizeof(stops[0]);
    if (elevDeg <= stops[0].e) return tft.color565(stops[0].r, stops[0].g, stops[0].b);
    for (int i = 1; i < n; i++) {
        if (elevDeg <= stops[i].e) {
            float f = (elevDeg - stops[i - 1].e) / (stops[i].e - stops[i - 1].e);
            uint8_t r = stops[i - 1].r + f * (stops[i].r - stops[i - 1].r);
            uint8_t g = stops[i - 1].g + f * (stops[i].g - stops[i - 1].g);
            uint8_t b = stops[i - 1].b + f * (stops[i].b - stops[i - 1].b);
            return tft.color565(r, g, b);
        }
    }
    return tft.color565(stops[n - 1].r, stops[n - 1].g, stops[n - 1].b);
}

// 3px daylight gradient bar: the zone's local 00:00-24:00 mapped left to
// right, each column colored by the sun's real elevation at that moment
// today (same solar math as the day/night colors), with a white tick at the
// current time. Shows at a glance how deep into day or night each city is.
// Skipped for zones without preset coordinates.
static void renderDaylightBar(TFT_eSPI &gfx, int x, int y, int w, WorldClockZone &zone)
{
    float lat, lon;
    if (!getCityCoords(zone.timezone, lat, lon)) return;

    time_t local = zone.tz.now();
    time_t utcNow = UTC.now();
    long secOfDay = (long)hour(local) * 3600 + (long)minute(local) * 60 + second(local);

    for (int i = 0; i < w; i++) {
        long colSec = (long)i * 86400L / (w - 1);
        time_t colUtc = utcNow + (colSec - secOfDay);
        gfx.drawFastVLine(x + i, y, 3, daylightBarColor(solarElevationDeg(lat, lon, colUtc)));
    }

    // "Now" tick, with background-color gaps so it reads inside the bright
    // midday section too
    int tickX = x + (int)(secOfDay * (long)(w - 1) / 86400L);
    gfx.drawFastVLine(tickX - 1, y - 2, 7, clockBackgroundColor);
    gfx.drawFastVLine(tickX + 1, y - 2, 7, clockBackgroundColor);
    gfx.drawFastVLine(tickX, y - 2, 7, TFT_WHITE);
}

// Fraction (0..1) of the exchange's regular trading day already elapsed;
// false when the exchange is outside it (weekend, holiday, before open /
// after close). The span runs from the first REGULAR open to the last
// REGULAR close - the SSE lunch break stays inside the span - and half-day
// early closes truncate it, matching getMarketStatus.
static bool marketDayProgress(WorldClockZone &zone, float &frac)
{
    if (!zone.market.hasMarket) return false;

    time_t local = zone.tz.now();
    int wd = weekday(local);
    if (wd == 1 || wd == 7) return false; // weekend
    if (isMarketHoliday(zone.market.exchange, year(local), month(local), day(local))) return false;

    int openMin = -1, closeMin = -1;
    for (int i = 0; i < zone.market.sessionCount; i++) {
        const TradingSession &s = zone.market.sessions[i];
        if (s.sessionName != "REGULAR") continue;
        int start = s.openHour * 60 + s.openMinute;
        int end = s.closeHour * 60 + s.closeMinute;
        if (openMin < 0 || start < openMin) openMin = start;
        if (end > closeMin) closeMin = end;
    }
    if (openMin < 0) return false;

    int early = marketEarlyCloseMinutes(zone.market.exchange, year(local), month(local), day(local));
    if (early >= 0 && early < closeMin) closeMin = early;

    int nowMin = hour(local) * 60 + minute(local);
    if (closeMin <= openMin || nowMin < openMin || nowMin >= closeMin) return false;
    frac = (float)(nowMin - openMin) / (float)(closeMin - openMin);
    return true;
}

// What the quadrant's bottom status line should show right now: the market
// status only. Weather alerts use the larger weekday line above the date.
// `flashes` is true only for the time-sensitive market alerts that blink.
struct StatusLine
{
    String text;
    uint16_t color;
    bool flashes;
};

static StatusLine computeMarketStatusLine(WorldClockZone &zone)
{
    String mkt = zone.lastMarketStatus;
    return {mkt, getMarketStatusColor(mkt), shouldMessageFlash(mkt)};
}

// Refresh the zone's cached weather-alert text from the background task. Cheap
// enough for the paint paths (per minute / on data changes), and keeps the
// per-loop needsDynamicQuadrantAreaUpdate() check lock-free.
static void refreshZoneWeatherAlert(WorldClockZone &zone, int zoneIdx)
{
    zone.weatherAlert = projectConfig.weatherAlerts ? getZoneAlert(zoneIdx) : String("");
}

static bool showAlertOnDayLine(WorldClockZone &zone)
{
    return projectConfig.weatherAlerts && wxAlertShowingAlert && zone.weatherAlert.length() > 0;
}

static String fitTextToWidth(TFT_eSPI &gfx, const String &text, int maxWidth)
{
    if (gfx.textWidth(text) <= maxWidth) return text;

    String out = text;
    while (out.length() > 0 && gfx.textWidth(out + "...") > maxWidth) {
        out.remove(out.length() - 1);
    }
    return out.length() > 0 ? out + "..." : String("");
}

// Render one quadrant's full content (label, time, AM/PM, day/date, market
// status) with gfx primitives at offset (ox, oy). gfx is normally the
// off-screen quadSprite (ox = oy = 0), which is then pushed to the panel in
// one blit; when the sprite allocation failed it is tft itself with the
// quadrant's top-left corner as the offset. zoneIdx (0-3) keys the
// public-holiday lookup for this quadrant.
static void renderQuadrantContent(TFT_eSPI &gfx, int ox, int oy, WorldClockZone &zone, int zoneIdx)
{
    gfx.fillRect(ox, oy, quadrantWidth, quadrantHeight, clockBackgroundColor);

    // Optional divider grid between the quadrants (settings / web page). Each
    // quadrant draws its own shared edges - the left edge in the right column,
    // the top edge in the bottom row - so the lines are part of the quadrant
    // content and survive single-quadrant redraws and blits.
    if (projectConfig.showGrid) {
        if (zoneIdx == 1 || zoneIdx == 3) {
            gfx.drawFastVLine(ox, oy, quadrantHeight, TFT_DARKGREY);
        }
        if (zoneIdx == 2 || zoneIdx == 3) {
            gfx.drawFastHLine(ox, oy, quadrantWidth, TFT_DARKGREY);
        }
    }

    time_t local = zone.tz.now();
    DayPhase phase = zoneDayPhase(zone);
    uint16_t timeColor = getDayNightColor(zone);
    uint16_t labelColor = getDayNightLabelColor(zone);
    int centerX = ox + quadrantWidth / 2;

    // Accent border marking the home quadrant - the reference all the (+1)
    // day offsets are computed against. Drawn first so the bars/text win any
    // overlap along the edges.
    if (projectConfig.homeMarker && zoneIdx == 0) {
        gfx.drawRoundRect(ox, oy, quadrantWidth, quadrantHeight, 6, TFT_DARKCYAN);
    }

    // Sun/moon glyph in the top-left corner: the explicit day/night marker
    // (the text colors alone used to carry this meaning)
    if (projectConfig.dayNightIcons) {
        drawDayNightIcon(gfx, ox + 7, oy + 11, phase);
    }

    // Timezone label, top-center
    gfx.setTextFont(1);
    gfx.setTextSize(2);
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(labelColor, clockBackgroundColor);
    gfx.drawString(zone.name, centerX, oy + 5);

    // Time (HH:MM), centered between the label and the date block
    bool pm;
    String hhmm = formatHHMM(local, pm);
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(timeColor, clockBackgroundColor);
    if (projectConfig.smoothTimeFont) {
        // Anti-aliased 52pt digits (fontTimeDigits.h) instead of the blocky
        // pixel-doubled Font 4. The font's ascent equals the digit cap
        // height, so with TC_DATUM the digit tops land exactly on this y
        // (37px tall - clears the city name above and the daylight bar /
        // day line below).
        gfx.loadFont(TIME_DIGITS_VLW);
        gfx.drawString(hhmm, centerX, oy + 26);
        gfx.unloadFont();
    } else {
        gfx.setTextFont(clockFont);
        gfx.setTextSize(clockSize);
        gfx.drawString(hhmm, centerX, oy + 22);
    }

    // In 12-hour mode, an AM/PM indicator in the top-right of the quadrant
    if (!SHOW_24HOUR) {
        gfx.setTextFont(1);
        gfx.setTextSize(1);
        gfx.setTextDatum(TR_DATUM);
        gfx.setTextColor(timeColor, clockBackgroundColor);
        gfx.drawString(pm ? "PM" : "AM", ox + quadrantWidth - 4, oy + 6);
    }

    // Daylight gradient bar under the time. It needs a few extra rows, so the
    // day/date lines shift down slightly while it is enabled; with it off the
    // layout is exactly the classic one.
    int dayY = oy + quadrantHeight - 50;     // day name (size 2)
    int holidayY = oy + quadrantHeight - 46; // holiday day line (size 1)
    int dateY = oy + quadrantHeight - 30;    // date (size 2)
    if (projectConfig.daylightBar) {
        renderDaylightBar(gfx, centerX - 60, oy + 69, 120, zone);
        dayY = oy + 74;
        holidayY = oy + 78;
        dateY = oy + 92;
    }

    // Weather alerts share the weekday/holiday line, so refresh the cached
    // alert before composing that row. The bottom market line uses only market
    // status now.
    refreshZoneWeatherAlert(zone, zoneIdx);

    // Day name with the day-offset vs home (top-left quadrant). Compare actual
    // calendar dates (not bare day-of-month) so it stays correct across month
    // and year boundaries.
    static const char *dayNames[8] = {"", "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    String dayText = dayNames[weekday(local)];
    time_t homeTime = worldZones[0].tz.now();
    long dayDiff = daysFromCivil(year(local), month(local), day(local)) -
                   daysFromCivil(year(homeTime), month(homeTime), day(homeTime));
    if (dayDiff >= 1) {
        dayText += " (+1)"; // Ahead of home (tomorrow)
    } else if (dayDiff <= -1) {
        dayText += " (-1)"; // Behind home (yesterday)
    }

    char dateBuffer[12];
    if (NOT_US_DATE) {
        sprintf(dateBuffer, "%02d/%02d/%02d", day(local), month(local), year(local) % 100);
    } else {
        sprintf(dateBuffer, "%02d/%02d/%02d", month(local), day(local), year(local) % 100);
    }

    // Public holiday in this zone today? The day line turns gold and drops
    // to the smaller font so the holiday's name fits next to the day name;
    // the date below stays visible as on any other day.
    char holidayName[32];
    uint32_t todayYmd = (uint32_t)year(local) * 10000u +
                        (uint32_t)month(local) * 100u + (uint32_t)day(local);
    bool isHoliday = getHolidayName(zoneIdx, todayYmd, holidayName, sizeof(holidayName));

    gfx.setTextFont(1);
    gfx.setTextDatum(TC_DATUM);
    if (showAlertOnDayLine(zone)) {
        // Larger than the old bottom status line. Font 2 keeps common alerts
        // readable in the 160px quadrant; extra-long NWS event names are
        // trimmed by measured pixel width, not by character count.
        gfx.setTextFont(2);
        gfx.setTextSize(1);
        gfx.setTextColor(WX_ALERT_COLOR, clockBackgroundColor);
        gfx.drawString(fitTextToWidth(gfx, zone.weatherAlert, quadrantWidth - 8), centerX, dayY);
    } else if (isHoliday) {
        gfx.setTextColor(TFT_GOLD, clockBackgroundColor);
        String dayLine = dayText + " - " + holidayName;
        if (dayLine.length() > 26) {
            dayLine = dayLine.substring(0, 26); // keep it inside the quadrant
        }
        gfx.setTextSize(1);
        // Vertically centered in the slot the size-2 day name normally fills
        gfx.drawString(dayLine, centerX, holidayY);
    } else {
        gfx.setTextColor(labelColor, clockBackgroundColor);
        gfx.setTextSize(2);
        gfx.drawString(dayText, centerX, dayY);
    }
    gfx.setTextFont(1);
    gfx.setTextSize(2);
    // With the quadrant temperature enabled the date shifts left a little:
    // dead-centered, its last digit touches the condition dot of a two-digit
    // reading (date right edge and dot left edge both land on x+128).
    int dateX = projectConfig.quadWeather ? centerX - 8 : centerX;
    gfx.drawString(dateBuffer, dateX, dateY);

    // Current temperature on the right of the date line (the date is always
    // 8 chars, so this slot is guaranteed free). The condition rides
    // in a color dot next to a white reading; sub -9 degree readings are a
    // character wider, so they drop the dot and carry the condition in the
    // digit color instead. Data comes from the background fetch task that
    // already serves the weather face; nothing is shown until the first
    // fetch lands (or for zones without preset coordinates).
    if (projectConfig.quadWeather) {
        ZoneWeather w = getZoneWeather(zoneIdx);
        if (w.valid) {
            String temp = String(displayTemp(w.tempC));
            uint16_t condColor = weatherCodeColor(w.weatherCode);
            bool dotFits = temp.length() <= 2;
            uint16_t tempColor = dotFits ? TFT_WHITE : condColor;
            gfx.setTextFont(1);
            gfx.setTextSize(1);
            gfx.setTextDatum(TR_DATUM);
            gfx.setTextColor(tempColor, clockBackgroundColor);
            int tempRight = ox + quadrantWidth - 10;
            gfx.drawString(temp, tempRight, dateY + 4);
            gfx.drawCircle(ox + quadrantWidth - 7, dateY + 5, 2, tempColor); // degree mark
            if (dotFits) {
                gfx.fillCircle(tempRight - gfx.textWidth(temp) - 7, dateY + 8, 3, condColor);
            }
        }
    }

    // Bottom status line: market status only. The <=10-minute open/close
    // market alerts still flash - in the flash-off phase that line is simply
    // not drawn.
    StatusLine line = computeMarketStatusLine(zone);
    if (line.text.length() > 0 && (!line.flashes || flashState)) {
        gfx.setTextFont(1);
        gfx.setTextSize(1);
        gfx.setTextDatum(TC_DATUM);
        gfx.setTextColor(line.color, clockBackgroundColor);
        gfx.drawString(line.text, centerX, oy + quadrantHeight - 10);
    }

    // Trading-day progress along the bottom edge while the exchange is inside
    // regular hours: how much of the session is left, at a glance. Green like
    // the "OPEN" status text; absent outside regular hours.
    if (projectConfig.marketProgressBar) {
        float frac;
        if (marketDayProgress(zone, frac)) {
            int barWidth = 120;
            int barX = centerX - barWidth / 2;
            int barY = oy + quadrantHeight - 2;
            gfx.fillRect(barX, barY, barWidth, 2, 0x39E7 /* dim grey track */);
            gfx.fillRect(barX, barY, (int)(barWidth * frac + 0.5f), 2, TFT_GREEN);
        }
    }
}

bool shouldMessageFlash(String message)
{
    return (message.indexOf("CLOSE IN") != -1 || message.indexOf("OPEN IN") != -1);
}

void updateFlashState()
{
    unsigned long currentTime = millis();
    if (currentTime - lastFlashTime >= flashInterval) {
        flashState = !flashState;
        lastFlashTime = currentTime;
        flashJustChanged = true;
    }
}

// Advance the slow weekday/weather-alert alternation. Asymmetric dwell (day
// slot longer) keeps the clock's normal date context primary while still
// surfacing the alert. Sets wxAlertPhaseJustChanged so affected quadrants
// repaint.
void updateWeatherAlertPhase()
{
    unsigned long now = millis();
    unsigned long dwell = wxAlertShowingAlert ? WX_ALERT_ALERT_MS : WX_ALERT_DAY_MS;
    if (now - wxAlertPhaseSince >= dwell) {
        wxAlertShowingAlert = !wxAlertShowingAlert;
        wxAlertPhaseSince = now;
        wxAlertPhaseJustChanged = true;
    }
}

void resetFlashChangeFlag()
{
    flashJustChanged = false;
    wxAlertPhaseJustChanged = false;
}

// Repaint the lower dynamic area (day/date/weather/market rows) without
// redrawing the whole quadrant. Called between minute ticks when a flashing
// market alert toggles or the weekday/weather-alert alternation swaps.
void updateDynamicQuadrantArea(WorldClockZone &zone, int quadrantIndex)
{
    QuadrantPos quad = quadrants[quadrantIndex];
    const int bandY = quadrantHeight - 56;
    const int bandH = quadrantHeight - bandY;

    bool usedSprite = false;
    if (quadSpriteMutex) xSemaphoreTake(quadSpriteMutex, portMAX_DELAY);
    if (quadSpriteOk) {
        // Re-render the quadrant off-screen and push only the rows holding the
        // weekday/alert, date/weather, status line and progress bar.
        // Pixel-identical to a full redraw - correct text placement, clipped
        // at the quadrant edge, and grid / home-border pixels a direct clear
        // rect would notch are all repainted - without blitting the time rows.
        renderQuadrantContent(quadSprite, 0, 0, zone, quadrantIndex);
        quadSprite.pushSprite(quad.x, quad.y + bandY, 0, bandY, quadrantWidth, bandH);
        usedSprite = true;
    }
    if (quadSpriteMutex) xSemaphoreGive(quadSpriteMutex);
    if (usedSprite) return;

    // Degraded direct-to-panel path (sprite allocation failed). Clip to the
    // lower quadrant so a wide alert cannot spill over the neighbouring
    // quadrant (vpDatum=false keeps absolute screen coordinates).
    tft.setViewport(quad.x, quad.y + bandY, quadrantWidth, bandH, false);
    renderQuadrantContent(tft, quad.x, quad.y, zone, quadrantIndex);
    tft.resetViewport();
}

bool hasTimeChanged(WorldClockZone &zone)
{
    time_t local = zone.tz.now();
    static unsigned long lastDebugOutput = 0;
    unsigned long currentMillis = millis();

    // Check if timezone is returning valid time
    if (local < 1000000000) { // Before year 2001 - invalid timestamp
        // Force reinitialize timezone if it's giving invalid time
        if (zone.timezone.length() > 0) {
            Log.println("Invalid time detected for " + zone.name + ", reinitializing timezone...");
            zone.tz.setLocation(zone.timezone);
            local = zone.tz.now();
        }

        // If still invalid, force update anyway to show something
        if (local < 1000000000) {
            Log.println("Still invalid time for " + zone.name + ", forcing update");
            zone.initialized = false; // Force redraw
            return true;
        }
    }

    int currentHour = hour(local); // Always use 24-hour format
    int currentMinute = minute(local);
    int currentDay = day(local);

    // Debug output every 10 seconds for the first zone only to avoid spam
    if (DEBUG_CLOCK && zone.name == "SANTA CLARA" && currentMillis - lastDebugOutput >= 10000) {
        CLOCK_DEBUG_PRINTLN("Zone " + zone.name + " - Current: " + String(currentHour) + ":" +
                      String(currentMinute) + ", Last: " + String(zone.lastHour) + ":" +
                      String(zone.lastMinute) + ", Initialized: " + String(zone.initialized));
        lastDebugOutput = currentMillis;
    }

    // Recompute the market status at most once per minute. getMarketStatus() does
    // heavy String work, and session transitions only happen on minute
    // boundaries, so there is no need to rebuild it on every loop iteration.
    bool minuteChanged = (!zone.initialized ||
                          zone.lastMinute != currentMinute ||
                          zone.lastDay != currentDay);
    bool marketStatusChanged = false;
    if (zone.market.hasMarket && minuteChanged) {
        String currentMarketStatus = getMarketStatus(zone);
        if (currentMarketStatus != zone.lastMarketStatus) {
            zone.lastMarketStatus = currentMarketStatus;
            marketStatusChanged = true;
            CLOCK_DEBUG_PRINTLN("Market status changed for " + zone.name + ": " + currentMarketStatus);
        }
    }

    bool timeChanged = (!zone.initialized ||
                       zone.lastHour != currentHour ||
                       zone.lastMinute != currentMinute ||
                       zone.lastDay != currentDay ||
                       marketStatusChanged);

    if (timeChanged) {
        if (DEBUG_CLOCK && zone.name == "SANTA CLARA" && zone.initialized) {
            CLOCK_DEBUG_PRINTLN("Time changed for " + zone.name + " from " +
                          String(zone.lastHour) + ":" + String(zone.lastMinute) +
                          " to " + String(currentHour) + ":" + String(currentMinute));
        }

        zone.lastHour = currentHour;
        zone.lastMinute = currentMinute;
        zone.lastDay = currentDay;
        zone.initialized = true;
    }

    return timeChanged;
}

// Whether the lower dynamic area needs a mid-minute repaint this frame: either
// a flashing market alert toggled, or the weekday/weather-alert alternation
// just swapped for a zone with an alert. Uses only cached values (no locks, no
// String rebuilds) so it is cheap to call every loop for all four zones.
bool needsDynamicQuadrantAreaUpdate(WorldClockZone &zone)
{
    bool hasMkt = zone.lastMarketStatus.length() > 0;
    bool hasAlert = projectConfig.weatherAlerts && zone.weatherAlert.length() > 0;

    // The swap changes the weekday line for any zone carrying an alert.
    if (wxAlertPhaseJustChanged && hasAlert) return true;

    // Weather alerts no longer occupy the market line, so market countdown
    // flashes whenever the market line carries a flashing status.
    if (flashJustChanged) {
        if (hasMkt && shouldMessageFlash(zone.lastMarketStatus)) return true;
    }
    return false;
}

/*-------- Ambient-light auto-brightness ----------*/
// Primary source: the CYD's onboard LDR (self-calibrating, see below).
// Fallback while the sensor is unproven (or disabled): the old fixed schedule
// on home-zone time. Manual changes (touch / settings / serial) always win
// for MANUAL_BRIGHTNESS_HOLD_MS via manualBrightnessUntil.

#if USE_LDR_AUTOBRIGHTNESS
static int ldrLastRaw = 0;    // last raw sample, normalized so higher = darker
static float ldrEma = -1.0f;  // smoothed reading (-1 until first sample)
static int ldrMinSeen = 4095; // observed range since boot - used both to
static int ldrMaxSeen = 0;    // self-calibrate and to prove the sensor works
static bool ldrDark = false;

// Sample every couple of seconds and classify the room as dark/bright with
// hysteresis around the midpoint of the observed range. The LDR circuit on
// some CYD revisions is broken (flat readings); until the range has swung by
// LDR_MIN_SWING the sensor is not trusted and ldrIsTrusted() stays false.
static void ldrSample(unsigned long now)
{
    static unsigned long lastSample = 0;
    if (now - lastSample < 2000) return;
    lastSample = now;

    int raw = analogRead(LDR_PIN);
#if !LDR_DARK_IS_HIGH
    raw = 4095 - raw; // normalize: higher = darker
#endif
    ldrLastRaw = raw;
    ldrEma = (ldrEma < 0) ? raw : ldrEma + 0.2f * (raw - ldrEma);

    int e = (int)ldrEma;
    if (e < ldrMinSeen) ldrMinSeen = e;
    if (e > ldrMaxSeen) ldrMaxSeen = e;

    if (ldrMaxSeen - ldrMinSeen >= LDR_MIN_SWING) {
        int mid = (ldrMinSeen + ldrMaxSeen) / 2;
        int band = (ldrMaxSeen - ldrMinSeen) / 8; // hysteresis, no flapping
        if (!ldrDark && e > mid + band) {
            ldrDark = true;
            Log.println("Auto brightness: room went dark (LDR " + String(e) + ")");
        } else if (ldrDark && e < mid - band) {
            ldrDark = false;
            Log.println("Auto brightness: room went bright (LDR " + String(e) + ")");
        }
    }
}

static bool ldrIsTrusted()
{
    return ldrMaxSeen - ldrMinSeen >= LDR_MIN_SWING;
}
#endif // USE_LDR_AUTOBRIGHTNESS

void printLdrStatus()
{
#if USE_LDR_AUTOBRIGHTNESS
    Log.println("=== LDR (ambient light sensor, GPIO " + String(LDR_PIN) + ") ===");
    Log.println("Raw (normalized, higher = darker): " + String(ldrLastRaw));
    Log.println("Smoothed: " + String((int)ldrEma));
    Log.println("Range seen since boot: " + String(ldrMinSeen) + " - " + String(ldrMaxSeen));
    if (ldrIsTrusted()) {
        Log.println("Sensor trusted: yes | Room: " + String(ldrDark ? "DARK" : "BRIGHT"));
    } else {
        Log.println("Sensor trusted: not yet (swing < " + String(LDR_MIN_SWING) +
                       " counts; using the " + String(projectConfig.nightStartHour) +
                       "-" + String(projectConfig.nightEndHour) +
                       "h night window fallback). Cover the sensor");
        Log.println("or shine a light at it - if the value never moves, this board's");
        Log.println("LDR circuit is one of the known-bad CYD revisions.");
    }
#else
    Log.println("LDR auto-brightness disabled at compile time (USE_LDR_AUTOBRIGHTNESS=0)");
#endif
}

bool getLdrState(bool &trusted, bool &dark, int &smoothed)
{
#if USE_LDR_AUTOBRIGHTNESS
    trusted = ldrIsTrusted();
    dark = ldrDark;
    smoothed = (int)ldrEma;
    return true;
#else
    trusted = false;
    dark = false;
    smoothed = -1;
    return false;
#endif
}

void adjustBrightnessAuto()
{
    unsigned long currentTime = millis();

    // Don't fight a manual brightness change (touch / serial) - let it hold first
    if (currentTime < manualBrightnessUntil) {
        return;
    }

    static unsigned long lastUpdate = 0;
    if (currentTime - lastUpdate < 250) return;
    lastUpdate = currentTime;

    int target = -1;
    int nightTarget = clampBrightness(projectConfig.nightBrightness);
    int dayTarget = clampBrightness(projectConfig.brightness);

#if USE_LDR_AUTOBRIGHTNESS
    // Sample even while auto-brightness is switched off so the sensor stays
    // calibrated (and the status pages stay live) for when it's re-enabled.
    ldrSample(currentTime);
#endif

    // Master switch (web settings page): off = ignore the light sensor and
    // the night schedule, hold the user's set brightness (fading back to it
    // if a dim was in effect when the switch was flipped).
    if (!projectConfig.autoBrightness) {
        target = dayTarget;
    }
#if USE_LDR_AUTOBRIGHTNESS
    else if (ldrIsTrusted()) {
        target = ldrDark ? nightTarget : dayTarget;
    }
#endif

    // Fallback: schedule on home-zone time. The window is configurable from
    // the web settings page (default 1-7 AM) and may wrap midnight; equal
    // start/end hours disable it.
    if (target < 0) {
        if (!worldZones[0].initialized) return;
        int currentHour = hour(worldZones[0].tz.now());
        int s = projectConfig.nightStartHour;
        int e = projectConfig.nightEndHour;
        bool night = (s != e) &&
                     (s < e ? (currentHour >= s && currentHour < e)
                            : (currentHour >= s || currentHour < e));
        target = night ? nightTarget : dayTarget;
    }

    // Fade toward the target instead of jumping (full sweep in a few seconds)
    if (backlightLevel != target) {
        int step = 8;
        if (backlightLevel < target) {
            backlightLevel = min(backlightLevel + step, target);
        } else {
            backlightLevel = max(backlightLevel - step, target);
        }
        analogWrite(BACKLIGHT_PIN, backlightLevel);
    }
}


void DrawSingleTimeZone(WorldClockZone &zone, int quadrantIndex)
{
    CLOCK_DEBUG_PRINTLN("Drawing " + zone.name + " (quadrant " + String(quadrantIndex) + ")");

    QuadrantPos quad = quadrants[quadrantIndex];
    bool usedSprite = false;
    if (quadSpriteMutex) xSemaphoreTake(quadSpriteMutex, portMAX_DELAY);
    if (quadSpriteOk) {
        // Compose off-screen, then update the panel in a single blit - the
        // quadrant never shows an intermediate (cleared / half-drawn) state.
        renderQuadrantContent(quadSprite, 0, 0, zone, quadrantIndex);
        quadSprite.pushSprite(quad.x, quad.y);
        usedSprite = true;
    }
    if (quadSpriteMutex) xSemaphoreGive(quadSpriteMutex);
    if (!usedSprite) renderQuadrantContent(tft, quad.x, quad.y, zone, quadrantIndex);
}

bool clockReleaseRenderBufferForNetwork()
{
    ensureQuadSpriteMutex();
    if (!quadSpriteMutex) return false;

    xSemaphoreTake(quadSpriteMutex, portMAX_DELAY);
    bool released = quadSpriteOk;
    if (released) {
        quadSprite.deleteSprite();
        quadSpriteOk = false;
    }
    xSemaphoreGive(quadSpriteMutex);
    return released;
}

void clockRestoreRenderBufferForNetwork(bool released)
{
    if (!released || !quadSpriteWanted) return;
    ensureQuadSpriteMutex();
    if (!quadSpriteMutex) return;

    xSemaphoreTake(quadSpriteMutex, portMAX_DELAY);
    bool ok = createQuadSpriteLocked();
    xSemaphoreGive(quadSpriteMutex);
    if (!ok) {
        Log.println("WARNING: quadrant sprite restore failed after network fetch");
    }
}

void showBrightnessBar(int brightness)
{
    // Draw brightness bar in center of screen
    int barWidth = 200;
    int barHeight = 20;
    int barX = (320 - barWidth) / 2;  // Center horizontally
    int barY = 110;  // Center vertically

    // Clear area around the bar
    tft.fillRect(barX - 10, barY - 30, barWidth + 20, barHeight + 60, clockBackgroundColor);

    // Draw "BRIGHTNESS" label
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("BRIGHTNESS", 160, barY - 20);

    // Draw outer border
    tft.drawRect(barX - 2, barY - 2, barWidth + 4, barHeight + 4, TFT_WHITE);

    // Fill background (empty part)
    tft.fillRect(barX, barY, barWidth, barHeight, TFT_BLACK);

    // Calculate fill width based on the supported brightness range.
    int fillWidth = map(clampBrightness(brightness), BRIGHTNESS_MIN, BRIGHTNESS_MAX, 0, barWidth);

    // Draw filled portion with gradient-like effect
    uint16_t fillColor;
    if (brightness < 85) {
        fillColor = TFT_RED;     // Low brightness - red
    } else if (brightness < 170) {
        fillColor = TFT_YELLOW;  // Medium brightness - yellow
    } else {
        fillColor = TFT_GREEN;   // High brightness - green
    }

    if (fillWidth > 0) {
        tft.fillRect(barX, barY, fillWidth, barHeight, fillColor);
    }

    // Draw percentage text
    int percentage = brightnessPercent(brightness);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(String(percentage) + "%", 160, barY + barHeight + 10);
}

void rollingClockSetup(bool is24Hour, bool usDate)
{
    Log.println("World Clock Setup");
    SHOW_24HOUR = is24Hour;
    // usDate == true  -> MM/DD/YY (US),  usDate == false -> DD/MM/YY (rest of world)
    NOT_US_DATE = !usDate;
    SetupCYD();
    // SetupCYD cleared the panel - put the boot console + Settings button
    // back for the zone-setup steps below.
    bootUiRefresh();

    // Off-screen quadrant buffer for flicker-free updates (see quadSprite)
    ensureQuadSpriteMutex();
    if (quadSpriteMutex) xSemaphoreTake(quadSpriteMutex, portMAX_DELAY);
    quadSpriteWanted = true;
    bool spriteReady = createQuadSpriteLocked();
    if (quadSpriteMutex) xSemaphoreGive(quadSpriteMutex);
    if (!spriteReady) {
        Log.println("WARNING: quadrant sprite allocation failed - drawing direct to panel");
    }

    // Initialize touch screen
    touchscreen.begin();
    Log.println("Touch screen initialized");

#if USE_LDR_AUTOBRIGHTNESS
    // 0 dB attenuation: the CYD's LDR divider only produces small voltages,
    // so the narrower ADC range (~0-950mV) gives usable resolution.
    analogSetPinAttenuation(LDR_PIN, ADC_0db);
#endif

    // Initialize backlight pin with PWM, restoring the saved brightness
    backlightLevel = clampBrightness(projectConfig.brightness);
    pinMode(BACKLIGHT_PIN, OUTPUT);
    analogWrite(BACKLIGHT_PIN, backlightLevel);

    Log.println(WiFi.status() == WL_CONNECTED
                    ? "Setting up the 4 time zones..."
                    : "Setting up the 4 time zones (no WiFi - cache / built-in rules)...");
    bootUiPoll();

    // Apply the timezones saved in the project config to the four quadrants
    // (falls back to the compiled-in defaults if nothing was saved yet).
    applyConfiguredZones();

    // Initialize all timezones. Each zone owns a slot in the emulated EEPROM
    // (ezTime's EZTIME_CACHE_EEPROM mechanism, EEPROM_CACHE_LEN bytes per slot)
    // so the timezone rules from a previous boot are still available when the
    // timezone server or the network is unreachable. The network is only hit on
    // a cache miss, or inside ezTime itself when the cached entry is older than
    // 6 months.
    for (int i = 0; i < 4; i++) {
        bool tzSuccess = false;

        // The cache payload is stored uppercased, so compare names
        // case-insensitively to check the slot holds the configured zone.
        if (worldZones[i].tz.setCache(i * EEPROM_CACHE_LEN) &&
            worldZones[i].tz.getOlson().equalsIgnoreCase(worldZones[i].timezone)) {
            tzSuccess = true;
            Log.println("CACHED: " + worldZones[i].name + " - " + worldZones[i].timezone);
            bootUiPoll();
        }

        int retryCount = 0;
        const int maxRetries = 5;
        // When the boot Settings button cut the boot short there is no
        // internet worth retrying against - skip straight to the POSIX
        // fallback below so the settings page appears without a long stall.
        while (!tzSuccess && retryCount < maxRetries && !bootUiPoll()) {
            Log.println("Fetching timezone " + worldZones[i].name + " - " +
                        worldZones[i].timezone + " (attempt " +
                        String(retryCount + 1) + "/" + String(maxRetries) + ")");
            bootUiPoll(); // paint the line before the blocking fetch

            // setLocation() returning true means the server sent a valid
            // definition and the POSIX rules were applied (and written to this
            // zone's EEPROM cache slot, assigned by setCache above). Note: don't
            // "verify" by comparing local time against UTC - zones at UTC+0
            // (e.g. London in winter) legitimately match UTC.
            bool fetched = worldZones[i].tz.setLocation(worldZones[i].timezone);
            if (fetched &&
                worldZones[i].tz.getOlson().equalsIgnoreCase(worldZones[i].timezone)) {
                tzSuccess = true;

                time_t local = worldZones[i].tz.now();
                Log.print("SUCCESS: ");
                Log.print(worldZones[i].name);
                Log.print(" - ");
                Log.print(worldZones[i].timezone);
                Log.print(" | Time: ");
                Log.print(hour(local));
                Log.print(":");
                if (minute(local) < 10) Log.print("0");
                Log.println(minute(local));
            } else {
                if (fetched) {
                    Log.println("FAILED: timezone server returned " +
                                worldZones[i].tz.getOlson() + " for " +
                                worldZones[i].timezone);
                } else {
                    Log.println("FAILED: timezone server gave no usable answer");
                }
                retryCount++;
                if (retryCount < maxRetries) {
                    Log.println("Retrying in 2 seconds...");
                    // Pump the console/Settings button through the pause
                    for (int t = 0; t < 40 && !bootUiPoll(); t++) {
                        delay(50);
                    }
                }
            }
        }

        if (!tzSuccess) {
            Log.print("ERROR: Failed to fetch timezone for ");
            Log.print(worldZones[i].name);
            Log.println(" after all retries!");

            // Timezone server unreachable and nothing usable cached. Preset
            // timezones carry built-in POSIX rules (incl. DST), so the zone
            // still shows correct local time; only a non-preset zone with a
            // stale cache has to fall back to UTC.
            const char *posix = getPosixFallback(worldZones[i].timezone);
            if (posix) {
                worldZones[i].tz.setPosix(posix);
                Log.println("Using built-in POSIX rules: " + String(posix));
            } else if (!worldZones[i].tz.getOlson().equalsIgnoreCase(worldZones[i].timezone)) {
                // The cache slot held a *different* zone (changed while
                // offline) - don't keep ticking with the wrong rules.
                worldZones[i].tz.setPosix("UTC");
                Log.println(worldZones[i].name + ": no rules available - falling back to UTC");
            }
            bootUiPoll();
        }

        // Seed the market-status cache so the first frame shows it immediately
        // (it is refreshed once per minute afterwards in hasTimeChanged).
        if (worldZones[i].market.hasMarket) {
            worldZones[i].lastMarketStatus = getMarketStatus(worldZones[i]);
        }
    }

    // Start the background fetchers now that the zones are configured: the
    // public-holiday tables first (the weather task on core 0 also services
    // them), then the weather fetch task itself.
    holidaysBegin();
    weatherBegin();
    // Remote log push (no-op unless LOG_PUSH_URL is set in secrets.h). The
    // boot lines logged before this point are already queued and ship once
    // the first NTP sync anchors their timestamps.
    logShipperBegin();

    Log.println("World clock ready - starting the display");
    bootUiPoll();

    // Show available serial commands
    showStartupCommands();
}

TouchPoint readTouchPoint()
{
    TouchPoint t = touchscreen.getTouch();
    if (projectConfig.flipDisplay) {
        // Rotation 3 mirrors both axes relative to the touch panel's fixed
        // orientation (calibrated for rotation 1).
        t.x = 320 - 1 - t.x;
        t.y = 240 - 1 - t.y;
    }
    return t;
}

void handleTouch()
{
    static unsigned long lastTouchTime = 0;

    TouchPoint touch = readTouchPoint();
    bool down = (touch.zRaw > 800); // zRaw indicates pressure

    if (!down)
    {
        touchSuppressedUntilRelease = false;
    }
    else if (!touchSuppressedUntilRelease)
    {
        unsigned long currentTime = millis();

        // getTouch() already maps touch.x into screen pixels (0..screenWidth),
        // so the 320px screen splits into three touch zones:
        //   left third  = dimmer, center third = settings, right third = brighter
        if (touch.x >= 107 && touch.x <= 213)
        {
            // Center tap opens the settings page. switchToScreen suppresses
            // further touch input until the finger is lifted.
            Log.println("CENTER touch - opening settings page");
            switchToScreen(SCREEN_SETTINGS);
            brightnessBarVisible = false;
            return;
        }

        // Debounce - only allow one touch every 10ms for brightness control
        if (currentTime - lastTouchTime > 10)
        {
            if (touch.x < 107) // Left third - make dimmer
            {
                backlightLevel = clampBrightness(backlightLevel - 1);

                Log.print("LEFT touch - Dimmer: ");
                Log.println(backlightLevel);
            }
            else // Right third - make brighter
            {
                backlightLevel = clampBrightness(backlightLevel + 1);

                Log.print("RIGHT touch - Brighter: ");
                Log.println(backlightLevel);
            }

            // Apply PWM to backlight pin
            analogWrite(BACKLIGHT_PIN, backlightLevel);

            // Hold this manual setting before auto-brightness resumes
            manualBrightnessUntil = currentTime + MANUAL_BRIGHTNESS_HOLD_MS;

            // Show brightness bar
            showBrightnessBar(backlightLevel);
            brightnessBarVisible = true;
            brightnessBarShownTime = currentTime;

            lastTouchTime = currentTime;

            Log.print("Touch at X: ");
            Log.print(touch.x);
            Log.print(", Y: ");
            Log.print(touch.y);
            Log.print(", Pressure: ");
            Log.print(touch.zRaw);
            Log.print(", Brightness: ");
            Log.println(backlightLevel);
        }
    }

    // Hide brightness bar after the configured timeout
    if (brightnessBarVisible && (millis() - brightnessBarShownTime > BRIGHTNESS_BAR_TIMEOUT_MS)) {
        brightnessBarVisible = false;
        // Persist the new level once, now that the adjustment gesture is over
        // (saving on every ±1 tick while the finger is held would hammer flash).
        if (projectConfig.brightness != backlightLevel) {
            projectConfig.brightness = backlightLevel;
            projectConfig.saveConfigFile();
        }
        // Clear the brightness bar area and force a full screen redraw
        tft.fillScreen(clockBackgroundColor);
        firstDraw = true; // This will force a complete redraw in the next cycle
        for (int i = 0; i < 4; i++) {
            worldZones[i].initialized = false;
        }
    }
}

// Steady (non-blinking) status label at the bottom-center of the home faces:
//  - "WIFI LOGIN REQUIRED" (orange) when associated but walled off behind a
//    captive portal (netCheck.cpp) - the clock is on WiFi but has no internet
//    until someone logs the network in; see http://<device-ip>/wifi-login.
//  - "NO WIFI" (red) when the connection has been gone for over a minute
//    (wifiWatch.cpp handles the reconnect kicks / self-heal reboot).
// Captive takes precedence over offline. Redrawn on a short cadence because the
// faces repaint their own regions each minute and would erase it; when the
// condition clears, a full repaint restores whatever the label covered.
static void serviceWifiIndicator()
{
    static bool drawn = false;
    static unsigned long lastDrawMs = 0;
    static const char *shownLabel = nullptr;

    bool captive = captivePortalActive();
    bool offline = wifiOfflineDurationMs() >= WIFI_INDICATOR_AFTER_MS;
    // The captive label doubles as a signpost to the fix: tapping the center
    // of the screen opens Settings, where the "WiFi login" helper lives.
    const char *label = captive ? "WIFI LOGIN REQUIRED - TAP CENTER > WIFI LOGIN"
                                : (offline ? "NO WIFI" : nullptr);

    if (label) {
        bool labelChanged = (shownLabel != label);
        if (labelChanged) {
            // Wipe the previous (possibly wider) label before the new one, via
            // a full repaint so no stray pixels are left behind.
            firstDraw = true;
            for (int i = 0; i < 4; i++) {
                worldZones[i].initialized = false;
            }
        }
        if (!drawn || labelChanged || millis() - lastDrawMs >= 250) {
            tft.setTextFont(1);
            tft.setTextSize(1);
            tft.setTextDatum(TC_DATUM);
            int w = tft.textWidth(label);
            // Clear rect + the 8px text cell both end at row 237: rows 238-239
            // belong to the bottom quadrants' market progress bars.
            tft.fillRect(160 - w / 2 - 2, 230, w + 4, 8, clockBackgroundColor);
            tft.setTextColor(captive ? TFT_ORANGE : TFT_RED, clockBackgroundColor);
            tft.drawString(label, 160, 230);
            drawn = true;
            shownLabel = label;
            lastDrawMs = millis();
        }
    } else if (drawn) {
        drawn = false;
        shownLabel = nullptr;
        firstDraw = true; // full repaint to restore what the label covered
        for (int i = 0; i < 4; i++) {
            worldZones[i].initialized = false;
        }
    }
}

void drawRollingClock()
{
    // Handle serial commands
    handleSerialCommands();

    // Update flash state for market status messages, and advance the slow
    // weekday/weather-alert alternation on the quadrant day line.
    updateFlashState();
    updateWeatherAlertPhase();

    // The web /wifi-login page can ask (from any screen) to open the on-device
    // captive-portal login helper; honour it here on the main core.
    if (wifiRelayRequested() && uiScreen != SCREEN_WIFI_LOGIN)
    {
        openWifiLoginHelper();
    }

    // If a settings/status/timezone page is open, it owns the screen and the
    // touch input; the clock quadrants resume when the user navigates back.
    if (uiScreen != SCREEN_HOME)
    {
        handleUiTouch();
        renderUiPage();
        resetFlashChangeFlag();
        return;
    }

    // Handle touch input for backlight control and opening the settings page
    handleTouch();
    if (uiScreen != SCREEN_HOME)
    {
        // A center tap just opened the settings page - render it next loop
        resetFlashChangeFlag();
        return;
    }

    // Ambient-light auto-brightness (time-of-day fallback inside)
    adjustBrightnessAuto();

    // Alternate faces (big clock / calendar / weather) own the whole screen;
    // the classic four-quadrant world clock is drawn by the code below.
    if (projectConfig.clockFace != FACE_QUAD) {
        drawAlternateFace();
        serviceWifiIndicator();
        resetFlashChangeFlag();
        return;
    }

    // Repaint the quadrants as soon as async public-holiday data lands, so a
    // freshly booted clock shows today's holiday without waiting for the
    // next minute tick.
    static uint32_t lastQuadHolidayVersion = 0;
    uint32_t holidayVersion = holidaysDataVersion();
    if (holidayVersion != lastQuadHolidayVersion) {
        lastQuadHolidayVersion = holidayVersion;
        for (int i = 0; i < 4; i++) {
            worldZones[i].initialized = false;
        }
    }

    // Likewise for the quadrant temperatures and weather alerts: repaint as
    // soon as the background weather task delivers fresh data instead of
    // waiting for the next minute tick.
    if (projectConfig.quadWeather || projectConfig.weatherAlerts) {
        static uint32_t lastQuadWeatherVersion = 0;
        uint32_t weatherVersion = weatherDataVersion();
        if (weatherVersion != lastQuadWeatherVersion) {
            lastQuadWeatherVersion = weatherVersion;
            for (int i = 0; i < 4; i++) {
                worldZones[i].initialized = false;
            }
        }
    }

    // Only clear the whole screen on first draw
    if (firstDraw) {
        tft.fillScreen(clockBackgroundColor);
        firstDraw = false;

        // Force redraw all quadrants on first run
        for (int i = 0; i < 4; i++) {
            DrawSingleTimeZone(worldZones[i], i);
        }
    } else {
        // Check each timezone for updates
        for (int i = 0; i < 4; i++) {
            if (hasTimeChanged(worldZones[i])) {
                // Full redraw needed (time, date, or market status changed)
                CLOCK_DEBUG_PRINTLN("Calling DrawSingleTimeZone for " + worldZones[i].name);
                DrawSingleTimeZone(worldZones[i], i);
            } else if (!brightnessBarVisible && needsDynamicQuadrantAreaUpdate(worldZones[i])) {
                // Only the lower dynamic rows changed - flashing market alert
                // toggled, or the weekday/weather-alert line swapped (skipped
                // while the brightness bar overlay owns the center of the screen).
                updateDynamicQuadrantArea(worldZones[i], i);
            }
        }
    }

    serviceWifiIndicator();

    // Reset flash change flag after all zones have been processed
    resetFlashChangeFlag();
}
