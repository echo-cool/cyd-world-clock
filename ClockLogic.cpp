/*-------- CYD (Cheap Yellow Display) ----------*/

#include "ClockLogic.h"

#include <WiFi.h> // link status for the zone-setup boot log

#include "brightness.h"
#include "clockFaces.h"
#include "holidayService.h"     // holidaysBegin, getHolidayName
#include "fontTimeDigits.h" // anti-aliased VLW digits for the quadrant times
#include "projectConfig.h"  // home-screen extras toggles
#include "serialCommands.h"
#include "timerFaces.h"     // stopwatch/countdown engines, alarm + reminders
#include "uiPages.h"
#include "weatherService.h" // weatherBegin
#include "logShipper.h"     // logShipperBegin - remote log push
#include "wifiWatch.h"      // offline indicator on the home faces
#include "netCheck.h"       // captivePortalActive - login-required indicator
#include "wifiRelay.h"      // wifiRelayRequested - web-triggered login helper
#include "timeFormat.h"     // puretime::formatHHMM - host-tested 12/24h formatting

// Off-screen buffer for one full clock quadrant (160x120 on CYD, 240x160 on
// Hosyond 4.0). Each
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

XPT2046_Bitbang touchscreen(BOARD_TOUCH_MOSI, BOARD_TOUCH_MISO,
                            BOARD_TOUCH_CLK, BOARD_TOUCH_CS,
                            BOARD_TOUCH_WIDTH, BOARD_TOUCH_HEIGHT);

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
void updateFlashState();
void updateWeatherAlertPhase();
void resetFlashChangeFlag();
void updateDynamicQuadrantArea(WorldClockZone &zone, int quadrantIndex);
bool needsDynamicQuadrantAreaUpdate(WorldClockZone &zone);

void SetupCYD()
{
    Log.println(String("Setup display: ") + BOARD_PROFILE_NAME);
    // tft.init();
    tft.fillScreen(clockBackgroundColor);
    tft.setTextColor(clockFontColor, clockBackgroundColor);

    tft.setRotation(projectConfig.flipDisplay ? BOARD_TFT_ROTATION_FLIPPED
                                              : BOARD_TFT_ROTATION_NORMAL);
    tft.setTextFont(clockFont);
    tft.setTextSize(clockSize);
    tft.setTextDatum(clockDatum);
}

// Screen dimensions for the selected board profile.
int screenWidth = BOARD_DISPLAY_WIDTH;
int screenHeight = BOARD_DISPLAY_HEIGHT;
int quadrantWidth = BOARD_DISPLAY_WIDTH / 2;
int quadrantHeight = BOARD_DISPLAY_HEIGHT / 2;

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
    {0, 0, BOARD_DISPLAY_WIDTH / 4, BOARD_DISPLAY_HEIGHT / 4},
    {BOARD_DISPLAY_WIDTH / 2, 0, BOARD_DISPLAY_WIDTH * 3 / 4, BOARD_DISPLAY_HEIGHT / 4},
    {0, BOARD_DISPLAY_HEIGHT / 2, BOARD_DISPLAY_WIDTH / 4, BOARD_DISPLAY_HEIGHT * 3 / 4},
    {BOARD_DISPLAY_WIDTH / 2, BOARD_DISPLAY_HEIGHT / 2,
     BOARD_DISPLAY_WIDTH * 3 / 4, BOARD_DISPLAY_HEIGHT * 3 / 4}
};

String formatHHMM(time_t local, bool &pm)
{
    // The 12/24-hour conversion is unit-tested in puretime::formatHHMM; here we
    // just feed it ezTime's already-extracted hour/minute.
    return String(puretime::formatHHMM(hour(local), minute(local), SHOW_24HOUR, pm).c_str());
}

// The sun-position / day-night logic (solar elevation, DayPhase, day/night
// colors and icons, sunrise/sunset, daylight bar) moved to solarPhase.cpp.

static void drawMiniSun(TFT_eSPI &gfx, int cx, int cy, uint16_t color)
{
    gfx.fillCircle(cx, cy, 3, color);
    gfx.drawLine(cx - 6, cy, cx - 5, cy, color);
    gfx.drawLine(cx + 5, cy, cx + 6, cy, color);
    gfx.drawLine(cx, cy - 6, cx, cy - 5, color);
    gfx.drawLine(cx, cy + 5, cx, cy + 6, color);
    gfx.drawLine(cx - 4, cy - 4, cx - 3, cy - 3, color);
    gfx.drawLine(cx + 3, cy - 3, cx + 4, cy - 4, color);
    gfx.drawLine(cx - 4, cy + 4, cx - 3, cy + 3, color);
    gfx.drawLine(cx + 3, cy + 3, cx + 4, cy + 4, color);
}

static void drawMiniCloud(TFT_eSPI &gfx, int cx, int cy, uint16_t color)
{
    gfx.fillCircle(cx - 4, cy + 1, 3, color);
    gfx.fillCircle(cx, cy - 1, 4, color);
    gfx.fillCircle(cx + 5, cy + 1, 3, color);
    gfx.fillRect(cx - 7, cy + 1, 15, 5, color);
}

static void drawMiniMoon(TFT_eSPI &gfx, int cx, int cy)
{
    gfx.fillCircle(cx, cy, 4, EVENING_TEXT_COLOR);
    gfx.fillCircle(cx + 3, cy - 1, 4, clockBackgroundColor); // carve the crescent
}

static void drawMiniSnowflake(TFT_eSPI &gfx, int cx, int cy, uint16_t color, int r)
{
    gfx.drawLine(cx - r, cy, cx + r, cy, color);
    gfx.drawLine(cx, cy - r, cx, cy + r, color);
    gfx.drawLine(cx - r + 1, cy - r + 1, cx + r - 1, cy + r - 1, color);
    gfx.drawLine(cx - r + 1, cy + r - 1, cx + r - 1, cy - r + 1, color);
}

// Three slanted streaks hanging from a cloud whose base sits at (cx, cy).
static void drawMiniRain(TFT_eSPI &gfx, int cx, int cy)
{
    gfx.drawLine(cx - 5, cy, cx - 7, cy + 4, TFT_CYAN);
    gfx.drawLine(cx, cy, cx - 2, cy + 5, TFT_CYAN);
    gfx.drawLine(cx + 5, cy, cx + 3, cy + 4, TFT_CYAN);
}

// Zigzag lightning bolt with its top at (cx, cy), reaching down to cy+7.
static void drawMiniBolt(TFT_eSPI &gfx, int cx, int cy)
{
    gfx.drawLine(cx, cy, cx - 3, cy + 4, TFT_ORANGE);
    gfx.drawLine(cx - 3, cy + 4, cx + 1, cy + 3, TFT_ORANGE);
    gfx.drawLine(cx + 1, cy + 3, cx - 2, cy + 7, TFT_ORANGE);
}

// 13x11 warning triangle with an exclamation mark carved in the background
// color, centered on (cx, cy). Sits ahead of the weather-alert text on the
// quadrant day line so the orange text reads as an alert, not a status.
static void drawMiniAlertIcon(TFT_eSPI &gfx, int cx, int cy, uint16_t color)
{
    int top = cy - 5, bot = cy + 5;
    gfx.fillTriangle(cx, top, cx - 6, bot, cx + 6, bot, color);
    gfx.drawFastVLine(cx, top + 3, 4, clockBackgroundColor); // exclamation stem
    gfx.drawPixel(cx, bot - 2, clockBackgroundColor);        // exclamation dot
}

// ~14px condition glyph for a WMO weather code, one distinct shape per code
// group (https://open-meteo.com/en/docs#weather_variable_documentation).
// `night` swaps the sun for a crescent moon on the clear / partly-cloudy /
// shower shapes so a clear 10 PM quadrant doesn't show a sun. Shared by the
// quadrant weather badge and the weather face rows.
void drawWeatherIcon(TFT_eSPI &gfx, int cx, int cy, int code, bool night)
{
    if (code == 0 || code == 1) { // clear / mostly clear
        if (night) drawMiniMoon(gfx, cx, cy);
        else drawMiniSun(gfx, cx, cy, TFT_YELLOW);
        return;
    }

    if (code == 2) { // partly cloudy: sun or moon peeking behind the cloud
        if (night) drawMiniMoon(gfx, cx - 4, cy - 3);
        else drawMiniSun(gfx, cx - 4, cy - 3, TFT_YELLOW);
        drawMiniCloud(gfx, cx + 2, cy + 1, TFT_LIGHTGREY);
        return;
    }

    if (code == 3) { // overcast
        drawMiniCloud(gfx, cx, cy, TFT_LIGHTGREY);
        return;
    }

    if (code == 45 || code == 48) { // fog: hazy bars
        uint16_t c = TFT_LIGHTGREY;
        gfx.drawFastHLine(cx - 7, cy - 3, 14, c);
        gfx.drawFastHLine(cx - 5, cy + 1, 12, c);
        gfx.drawFastHLine(cx - 7, cy + 5, 10, c);
        return;
    }

    if (code >= 51 && code <= 55) { // drizzle: sparse dots, lighter than rain
        drawMiniCloud(gfx, cx, cy - 2, TFT_LIGHTGREY);
        gfx.fillRect(cx - 5, cy + 4, 2, 2, TFT_CYAN);
        gfx.fillRect(cx - 1, cy + 6, 2, 2, TFT_CYAN);
        gfx.fillRect(cx + 3, cy + 4, 2, 2, TFT_CYAN);
        return;
    }

    if (code == 56 || code == 57 || code == 66 || code == 67) {
        // freezing drizzle/rain: a streak and a small flake side by side
        drawMiniCloud(gfx, cx, cy - 2, TFT_LIGHTGREY);
        gfx.drawLine(cx - 3, cy + 4, cx - 5, cy + 8, TFT_CYAN);
        drawMiniSnowflake(gfx, cx + 4, cy + 6, TFT_WHITE, 3);
        return;
    }

    if ((code >= 71 && code <= 77) || code == 85 || code == 86) { // snow
        drawMiniCloud(gfx, cx, cy - 2, TFT_LIGHTGREY);
        drawMiniSnowflake(gfx, cx, cy + 6, TFT_WHITE, 4);
        return;
    }

    if (code == 96 || code == 99) { // thunderstorm with hail: bolt + stones
        drawMiniCloud(gfx, cx, cy - 2, TFT_DARKGREY);
        drawMiniBolt(gfx, cx - 1, cy + 2);
        gfx.fillRect(cx - 7, cy + 5, 2, 2, TFT_WHITE);
        gfx.fillRect(cx + 4, cy + 4, 2, 2, TFT_WHITE);
        return;
    }

    if (code >= 95) { // thunderstorm
        drawMiniCloud(gfx, cx, cy - 2, TFT_DARKGREY);
        drawMiniBolt(gfx, cx - 1, cy + 2);
        return;
    }

    if (code >= 80 && code <= 82) { // rain showers: sun/moon behind the cloud
        if (night) drawMiniMoon(gfx, cx - 5, cy - 5);
        else drawMiniSun(gfx, cx - 5, cy - 5, TFT_YELLOW);
        drawMiniCloud(gfx, cx + 1, cy - 1, TFT_LIGHTGREY);
        drawMiniRain(gfx, cx + 1, cy + 5);
        return;
    }

    if (weatherCodeHasPrecip(code)) { // plain rain (61/63/65)
        drawMiniCloud(gfx, cx, cy - 2, TFT_LIGHTGREY);
        drawMiniRain(gfx, cx, cy + 4);
        return;
    }

    gfx.drawCircle(cx, cy, 5, TFT_DARKGREY); // unknown code
}


// The market/trading-session status logic (getMarketStatus and friends)
// moved to marketStatus.cpp.

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
    zone.weatherNotice = projectConfig.quadWeather ? getZonePrecipNotice(zoneIdx) : String("");
}

static bool showAlertOnDayLine(WorldClockZone &zone)
{
    return projectConfig.weatherAlerts && wxAlertShowingAlert && zone.weatherAlert.length() > 0;
}

static bool showNoticeOnDateLine(WorldClockZone &zone)
{
    return projectConfig.quadWeather && wxAlertShowingAlert && zone.weatherNotice.length() > 0;
}

uint16_t weatherNoticeColor(const String &notice)
{
    if (notice.startsWith("SNOW")) return TFT_WHITE;
    if (notice.startsWith("STORM")) return TFT_ORANGE;
    return TFT_CYAN;
}

String fitTextToWidth(TFT_eSPI &gfx, const String &text, int maxWidth)
{
    if (gfx.textWidth(text) <= maxWidth) return text;

    String out = text;
    while (out.length() > 0 && gfx.textWidth(out + "...") > maxWidth) {
        out.remove(out.length() - 1);
    }
    return out.length() > 0 ? out + "..." : String("");
}

static bool useLargeQuadrantLayout()
{
    return quadrantWidth >= 220 && quadrantHeight >= 150;
}

static void drawQuadrantTime(TFT_eSPI &gfx, const String &hhmm, int centerX,
                             int y, uint16_t color, bool largeLayout)
{
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(color, clockBackgroundColor);

    if (largeLayout && !projectConfig.smoothTimeFont) {
        // Use a narrower centered time than TFT_eSPI Font 8. Font 8 fills the
        // 240px quadrant almost edge-to-edge, which reads visually left-heavy
        // on this panel even when mathematically centered.
        gfx.setTextFont(clockFont);
        gfx.setTextSize(3);
        if (gfx.textWidth(hhmm) <= quadrantWidth - 24) {
            gfx.drawString(hhmm, centerX, y);
            return;
        }

        gfx.setTextSize(2);
        gfx.drawString(hhmm, centerX, y + 8);
        return;
    }

    if (projectConfig.smoothTimeFont) {
        // Anti-aliased 52pt digits (fontTimeDigits.h). The font was designed
        // for the 160x120 CYD quadrants, but still remains available on larger
        // panels when the user explicitly prefers the smooth style.
        gfx.loadFont(TIME_DIGITS_VLW);
        gfx.drawString(hhmm, centerX, largeLayout ? y + 8 : y + 4);
        gfx.unloadFont();
    } else {
        gfx.setTextFont(clockFont);
        gfx.setTextSize(clockSize);
        gfx.drawString(hhmm, centerX, largeLayout ? y + 8 : y);
    }
}

static void drawCenteredTextFit(TFT_eSPI &gfx, const String &text, int centerX, int y,
                                uint16_t color, int maxWidth,
                                int preferredSize, int minSize)
{
    gfx.setTextFont(1);
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(color, clockBackgroundColor);
    for (int size = preferredSize; size >= minSize; --size) {
        gfx.setTextSize(size);
        if (gfx.textWidth(text) <= maxWidth) {
            gfx.drawString(text, centerX, y);
            return;
        }
    }
    gfx.drawString(fitTextToWidth(gfx, text, maxWidth), centerX, y);
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
    bool largeLayout = useLargeQuadrantLayout();
    bool bottomRow = zoneIdx >= 2;
    int labelY = oy + (largeLayout ? (bottomRow ? 8 : 2)
                                   : (bottomRow ? 8 : 5));
    int timeY = oy + (largeLayout ? (bottomRow ? 32 : 29)
                                  : (bottomRow ? 24 : 22));

    // Accent border marking the home quadrant - the reference all the (+1)
    // day offsets are computed against. Drawn first so the bars/text win any
    // overlap along the edges.
    if (projectConfig.homeMarker && zoneIdx == 0) {
        gfx.drawRoundRect(ox, oy, quadrantWidth, quadrantHeight, 6, TFT_DARKCYAN);
    }

    // Sun/moon glyph in the top-left corner: the explicit day/night marker
    // (the text colors alone used to carry this meaning)
    if (projectConfig.dayNightIcons) {
        drawDayNightIcon(gfx, ox + (largeLayout ? 12 : 7),
                         oy + (largeLayout ? 14 : 11), phase);
    }

    // Timezone label, top-center. Large panels can afford a stronger city
    // header; long names fall back by measured pixel width.
    if (largeLayout) {
        drawCenteredTextFit(gfx, zone.name, centerX, labelY, labelColor,
                            quadrantWidth - 14, 3, 2);
    } else {
        gfx.setTextFont(1);
        gfx.setTextSize(2);
        gfx.setTextDatum(TC_DATUM);
        gfx.setTextColor(labelColor, clockBackgroundColor);
        gfx.drawString(zone.name, centerX, labelY);
    }

    // Time (HH:MM), centered between the label and the date block
    bool pm;
    String hhmm = formatHHMM(local, pm);
    drawQuadrantTime(gfx, hhmm, centerX, timeY, timeColor, largeLayout);

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
    // The large-layout rows sit 4px lower than they used to (and match the
    // daylight-bar variant's date slot below): size-3 time digits reach
    // ~56px below timeY, so the day row needs the extra clearance not to
    // brush against them.
    int dayY = oy + quadrantHeight - (largeLayout ? 64 : 50);     // day name
    int holidayY = oy + quadrantHeight - (largeLayout ? 60 : 46); // holiday day line
    int dateY = oy + quadrantHeight - (largeLayout ? 41 : 30);    // date/weather
    if (projectConfig.daylightBar) {
        int barWidth = largeLayout ? min(180, quadrantWidth - 40) : 120;
        int barY = largeLayout ? oy + 92 : oy + 69;
        renderDaylightBar(gfx, centerX - barWidth / 2, barY, barWidth, zone);
        dayY = largeLayout ? oy + 98 : oy + 74;
        holidayY = largeLayout ? oy + 103 : oy + 78;
        dateY = largeLayout ? oy + 119 : oy + 92;
    }

    // Weather alerts share the weekday/holiday line, so refresh the cached
    // alert before composing that row. The bottom market line uses only market
    // status now.
    refreshZoneWeatherAlert(zone, zoneIdx);

    // Day name with the day-offset vs home (top-left quadrant). Compare actual
    // calendar dates (not bare day-of-month) so it stays correct across month
    // and year boundaries.
    String dayText = DAY_NAMES[weekday(local)];
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
        // Warning triangles flanking the text mark the line as an active
        // alert at a glance. Both icons' widths are reserved before the
        // text is measured so the trio always fits the quadrant, and the
        // text stays centered with an icon mirrored on each side.
        const int alertIconW = 13, alertIconGap = 3;
        String alertText = fitTextToWidth(gfx, zone.weatherAlert,
                                          quadrantWidth - 8 - 2 * (alertIconW + alertIconGap));
        int iconOffset = gfx.textWidth(alertText) / 2 + alertIconGap + alertIconW / 2;
        drawMiniAlertIcon(gfx, centerX - iconOffset, dayY + 7, WX_ALERT_COLOR);
        drawMiniAlertIcon(gfx, centerX + iconOffset, dayY + 7, WX_ALERT_COLOR);
        gfx.drawString(alertText, centerX, dayY);
    } else if (isHoliday) {
        gfx.setTextColor(TFT_GOLD, clockBackgroundColor);
        String dayLine = dayText + " - " + holidayName;
        gfx.setTextSize(1);
        // Vertically centered in the slot the size-2 day name normally fills
        gfx.drawString(fitTextToWidth(gfx, dayLine, quadrantWidth - 8), centerX, holidayY);
    } else {
        gfx.setTextColor(labelColor, clockBackgroundColor);
        gfx.setTextSize(2);
        gfx.drawString(dayText, centerX, dayY);
    }
    gfx.setTextFont(1);
    gfx.setTextSize(2);
    if (showNoticeOnDateLine(zone)) {
        // The date/weather line alternates with near-term precipitation
        // changes. Each layout tries its regular row font first and steps
        // down one font when the whole string is too wide: the lead time
        // sits at the tail, so ellipsis-trimming would hide exactly the
        // part that matters ("STORM STOPS IN 2H4...").
        int noticeMax = quadrantWidth - (largeLayout ? 16 : 8);
        int noticeY = dateY + (largeLayout ? 0 : 1);
        gfx.setTextFont(largeLayout ? 1 : 2);
        gfx.setTextSize(largeLayout ? 2 : 1);
        if (gfx.textWidth(zone.weatherNotice) > noticeMax) {
            if (largeLayout) {
                gfx.setTextFont(2); // same 16px row height, narrower glyphs
            } else {
                gfx.setTextFont(1); // 6x8 GLCD, centered in the font-2 slot
                noticeY = dateY + 5;
            }
            gfx.setTextSize(1);
        }
        gfx.setTextDatum(TC_DATUM);
        gfx.setTextColor(weatherNoticeColor(zone.weatherNotice), clockBackgroundColor);
        gfx.drawString(fitTextToWidth(gfx, zone.weatherNotice, noticeMax),
                       centerX, noticeY);
    } else {
        // With quadrant weather enabled, large panels keep the date centered;
        // the weather badge moves to the day row so it does not pull the
        // primary time/date stack off-center. Small panels shift the date
        // left instead, making room for the icon + reading on the same row.
        int dateX = centerX;
        if (projectConfig.quadWeather) {
            dateX = largeLayout ? centerX - 34 : centerX - 16;
        }
        if (largeLayout && projectConfig.quadWeather) {
            gfx.setTextDatum(TC_DATUM);
            gfx.drawString(dateBuffer, centerX, dateY);
        } else {
            gfx.drawString(dateBuffer, dateX, dateY);
        }
    }

    // Current temperature on the right of the date line (day row on the large
    // layout), with a condition icon next to the white reading. Font 2 keeps
    // the small-layout reading the same height as the date beside it. The
    // icon is dropped - never overlapped - when a rare 3-char reading (-12,
    // 104) leaves no measured room for it. Data comes from the background
    // fetch task that already serves the weather face; nothing is shown until
    // the first fetch lands (or for zones without preset coordinates).
    // On the large layout the badge shares the day row with the alternating
    // weekday/weather-alert text; it sits out the alert phase so a centered
    // alert never runs into the icon and reading (the row swaps back within
    // seconds).
    if (projectConfig.quadWeather && !showNoticeOnDateLine(zone) &&
        !(largeLayout && showAlertOnDayLine(zone))) {
        ZoneWeather w = getZoneWeather(zoneIdx);
        if (w.valid) {
            String temp = String(displayTemp(w.tempC));
            gfx.setTextFont(largeLayout ? 1 : 2);
            gfx.setTextSize(largeLayout ? 2 : 1);
            gfx.setTextDatum(TR_DATUM);
            gfx.setTextColor(TFT_WHITE, clockBackgroundColor);
            int tempRight = ox + quadrantWidth - 10;
            int tempY = largeLayout ? dayY + 1 : dateY;
            gfx.drawString(temp, tempRight, tempY);
            gfx.drawCircle(tempRight + (largeLayout ? 4 : 3),
                           tempY + (largeLayout ? 4 : 3), 2, TFT_WHITE); // degree mark
            int iconCx = tempRight - gfx.textWidth(temp) - (largeLayout ? 17 : 9);
            // The day text is at most 8 fixed-width chars (96px at size 2)
            // centered on centerX (small layout: the date, centered on
            // centerX-16); the widest icons reach ~11px from their center.
            // The icon is dropped rather than drawn into that text.
            bool iconFits = largeLayout ? iconCx - 11 >= centerX + 52
                                        : iconCx - 11 >= centerX + 32;
            if (iconFits) {
                drawWeatherIcon(gfx, iconCx,
                                largeLayout ? tempY + 8 : dateY + 8,
                                w.weatherCode, phase != PHASE_DAY);
            }
        }
    }

    // Bottom status line: market status only. The <=10-minute open/close
    // market alerts still flash - in the flash-off phase that line is simply
    // not drawn.
    StatusLine line = computeMarketStatusLine(zone);
    if (line.text.length() > 0 && (!line.flashes || flashState)) {
        if (largeLayout) {
            // Font 1 at size 2 is the regular look; countdown texts that are
            // too wide for it ("HKEX OPENS IN 21H 41M") switch to Font 2 -
            // same 16px row height, narrower glyphs - instead of losing the
            // countdown tail to an ellipsis.
            gfx.setTextDatum(TC_DATUM);
            gfx.setTextColor(line.color, clockBackgroundColor);
            gfx.setTextFont(1);
            gfx.setTextSize(2);
            if (gfx.textWidth(line.text) > quadrantWidth - 8) {
                gfx.setTextFont(2);
                gfx.setTextSize(1);
            }
            // -21 (not -24): breathing room below the 16px date row, which
            // ends at -25 in both date-slot variants.
            gfx.drawString(fitTextToWidth(gfx, line.text, quadrantWidth - 8),
                           centerX, oy + quadrantHeight - 21);
        } else {
            gfx.setTextFont(1);
            gfx.setTextSize(1);
            gfx.setTextDatum(TC_DATUM);
            gfx.setTextColor(line.color, clockBackgroundColor);
            gfx.drawString(fitTextToWidth(gfx, line.text, quadrantWidth - 8),
                           centerX, oy + quadrantHeight - 12);
        }
    }

    // Trading-day progress along the bottom edge while the exchange is inside
    // regular hours: how much of the session is left, at a glance. Green like
    // the "OPEN" status text; absent outside regular hours.
    if (projectConfig.marketProgressBar) {
        float frac;
        if (marketDayProgress(zone, frac)) {
            int barWidth = largeLayout ? min(180, quadrantWidth - 40) : 120;
            int barX = centerX - barWidth / 2;
            int barY = oy + quadrantHeight - 2;
            gfx.fillRect(barX, barY, barWidth, 2, 0x39E7 /* dim grey track */);
            gfx.fillRect(barX, barY, (int)(barWidth * frac + 0.5f), 2, TFT_GREEN);
        }
    }
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
    const int bandY = useLargeQuadrantLayout() ? quadrantHeight - 78
                                               : quadrantHeight - 56;
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

void invalidateHomeScreen(bool clearScreen)
{
    if (clearScreen) tft.fillScreen(clockBackgroundColor);
    firstDraw = true;
    for (int i = 0; i < 4; i++) {
        worldZones[i].initialized = false;
    }
}

bool hasTimeChanged(WorldClockZone &zone)
{
    time_t local = zone.tz.now();
    static unsigned long lastDebugOutput = 0;
    unsigned long currentMillis = millis();

    // Check if timezone is returning valid time
    if (local < 1000000000) { // Before year 2001 - invalid timestamp
        // Retry the timezone setup, but never more than once per zone per
        // minute and only while WiFi is up: setLocation() blocks on UDP with
        // multi-second timeouts, and this runs in the draw path - unthrottled
        // retries (4 zones, every loop) freeze the touch UI exactly when NTP
        // cannot sync.
        if (zone.timezone.length() > 0 && WiFi.status() == WL_CONNECTED &&
            (zone.lastTzReinitMs == 0 ||
             currentMillis - zone.lastTzReinitMs >= TZ_REINIT_RETRY_MS)) {
            zone.lastTzReinitMs = currentMillis;
            Log.println("Invalid time detected for " + zone.name + ", reinitializing timezone...");
            zone.tz.setLocation(zone.timezone);
            local = zone.tz.now();
            if (local < 1000000000) {
                Log.println("Still invalid time for " + zone.name + ", forcing update");
            }
        }

        // If still invalid, force update anyway to show something
        if (local < 1000000000) {
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
    bool hasNotice = projectConfig.quadWeather && zone.weatherNotice.length() > 0;

    // The swap changes the weekday line for alerts and the date/weather line
    // for near-term rain/snow notices.
    if (wxAlertPhaseJustChanged && (hasAlert || hasNotice)) return true;

    // Weather alerts no longer occupy the market line, so market countdown
    // flashes whenever the market line carries a flashing status.
    if (flashJustChanged) {
        if (hasMkt && shouldMessageFlash(zone.lastMarketStatus)) return true;
    }
    return false;
}

// The backlight/brightness control (manual hold, LDR auto-brightness,
// brightness bar, gesture step) moved to brightnessControl.cpp.

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

    // Off-screen quadrant buffer for flicker-free updates (see quadSprite).
    // Larger boards can opt out to keep heap available for network fetchers.
    ensureQuadSpriteMutex();
    if (quadSpriteMutex) xSemaphoreTake(quadSpriteMutex, portMAX_DELAY);
    quadSpriteWanted = BOARD_USE_QUAD_SPRITE;
    bool spriteReady = !quadSpriteWanted || createQuadSpriteLocked();
    if (quadSpriteMutex) xSemaphoreGive(quadSpriteMutex);
    if (quadSpriteWanted && !spriteReady) {
        Log.println("WARNING: quadrant sprite allocation failed - drawing direct to panel");
    }

#if BOARD_TOUCH_DRIVER == BOARD_TOUCH_DRIVER_BITBANG
    // Initialize touch screen. Hosyond's touch shares the LCD SPI pins, so that
    // profile uses TFT_eSPI's SPI-transaction touch path instead.
    touchscreen.begin();
    if (touchCalibrationAvailable())
    {
        touchscreen.setCalibration(projectConfig.touchCal[0], projectConfig.touchCal[1],
                                   projectConfig.touchCal[2], projectConfig.touchCal[3]);
        Log.println("Touch screen initialized (bitbang, saved calibration)");
    }
    else
    {
        Log.println("Touch screen initialized (bitbang, NOT calibrated)");
    }
#else
    if (touchCalibrationAvailable())
    {
        tft.setTouch(projectConfig.touchCal);
        Log.println("Touch screen initialized (TFT_eSPI shared SPI, saved calibration)");
    }
    else
    {
        // tft.getTouch() runs on the library's example calibration until the
        // calibration screen has been completed once (it offers itself on the
        // first home-screen loop; see drawRollingClock).
        Log.println("Touch screen initialized (TFT_eSPI shared SPI, NOT calibrated)");
    }
#endif

#if USE_LDR_AUTOBRIGHTNESS
    // 0 dB attenuation: the CYD's LDR divider only produces small voltages,
    // so the narrower ADC range (~0-950mV) gives usable resolution.
    analogSetPinAttenuation(LDR_PIN, ADC_0db);
#endif

    // Initialize backlight pin with PWM, restoring the saved brightness
    backlightLevel = clampBrightness(projectConfig.brightness);
    backlightPinSetup();
    setBacklight(backlightLevel);

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

// Synthetic touch injection (/api/touch, otaUpdate.cpp). Coordinates are
// final screen pixels, so the flip mirroring below must NOT be applied to
// them - a caller working from a /screenshot always hits what they see.
static int injectedTouchX = 0;
static int injectedTouchY = 0;
static unsigned long injectedTouchUntil = 0; // millis deadline; 0 = idle

void injectTouchPoint(int x, int y, unsigned long holdMs)
{
    injectedTouchX = x;
    injectedTouchY = y;
    injectedTouchUntil = millis() + holdMs;
}

TouchPoint readTouchPoint()
{
    if (injectedTouchUntil != 0)
    {
        if ((long)(millis() - injectedTouchUntil) < 0)
        {
            return TouchPoint{(uint16_t)injectedTouchX, (uint16_t)injectedTouchY,
                              (uint16_t)injectedTouchX, (uint16_t)injectedTouchY,
                              2000};
        }
        injectedTouchUntil = 0; // hold expired -> the "finger" lifts
    }

#if BOARD_TOUCH_DRIVER == BOARD_TOUCH_DRIVER_TFT_ESPI
    uint16_t x = 0;
    uint16_t y = 0;
    TouchPoint t = tft.getTouch(&x, &y) ? TouchPoint{x, y, x, y, 1000}
                                        : TouchPoint{0, 0, 0, 0, 0};
#else
    TouchPoint t = touchscreen.getTouch();
#endif
    // Some drivers map the far ADC endpoint to width/height (one pixel past
    // the panel). Clamp before optional mirroring so flipped displays cannot
    // underflow an unsigned coordinate at the calibrated edge.
    if (t.zRaw > 0) {
        if (t.x >= screenWidth) t.x = screenWidth - 1;
        if (t.y >= screenHeight) t.y = screenHeight - 1;
    }
    if (projectConfig.flipDisplay) {
        // The flipped board rotation mirrors both axes relative to the touch
        // panel's fixed orientation.
        t.x = screenWidth - 1 - t.x;
        t.y = screenHeight - 1 - t.y;
    }
    return t;
}

void cycleClockFace(int step, const char *source)
{
    projectConfig.clockFace = (projectConfig.clockFace + step) % FACE_COUNT;
    // A brightness adjustment may still be pending its bar-timeout save;
    // fold it into this write instead of dropping it.
    projectConfig.brightness = backlightLevel;
    projectConfig.saveConfigFile();
    Log.println(String(source) + " - clock face: " +
                clockFaceName(projectConfig.clockFace));
    // Full repaint, same as returning home from a settings page.
    invalidateHomeScreen(true);
    brightnessBarVisible = false;
    touchSuppressedUntilRelease = true;
}

void handleTouch()
{
    TouchPoint touch = readTouchPoint();
    bool down = (touch.zRaw > 800); // zRaw indicates pressure

    if (!down)
    {
        touchSuppressedUntilRelease = false;
    }
    else if (!touchSuppressedUntilRelease)
    {
        // The timer faces draw visible buttons and retain brightness gestures
        // in otherwise-unused left/right areas. Route their taps there, then
        // continue to the shared brightness-overlay timeout below.
        if (projectConfig.clockFace == FACE_STOPWATCH ||
            projectConfig.clockFace == FACE_COUNTDOWN)
        {
            timerFaceHandleTouch(touch.x, touch.y);
        }
        else
        {
            // getTouch() already maps touch.x/y into screen pixels, so the screen
            // splits into touch zones:
            //   center third = settings
            //   lower-left / lower-right corner = previous / next clock face
            //   rest of the left/right thirds = dimmer / brighter
            int leftThird = screenWidth / 3;
            int rightThird = (screenWidth * 2) / 3;
            if (touch.x >= leftThird && touch.x <= rightThird)
            {
                // Center tap opens the settings page. switchToScreen suppresses
                // further touch input until the finger is lifted.
                Log.println("CENTER touch - opening settings page");
                switchToScreen(SCREEN_SETTINGS);
                brightnessBarVisible = false;
                return;
            }

            if (touch.y >= (screenHeight * 2) / 3)
            {
                // Corner tap cycles the clock face; one step per tap (input stays
                // suppressed until the finger is lifted, like a screen switch).
                cycleClockFace(touch.x < leftThird ? FACE_COUNT - 1 : 1,
                               touch.x < leftThird ? "LOWER-LEFT touch"
                                                   : "LOWER-RIGHT touch");
                return;
            }

            // Left/right third: held-finger dimmer/brighter gesture.
            brightnessGestureStep(touch.x < leftThird ? -1 : +1);
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
        invalidateHomeScreen(true);
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

    // The timer faces' bottom button row occupies the indicator's slot; they
    // skip the label (face switches repaint the whole screen, so nothing
    // stale is left behind).
    if (projectConfig.clockFace == FACE_STOPWATCH ||
        projectConfig.clockFace == FACE_COUNTDOWN)
    {
        drawn = false;
        shownLabel = nullptr;
        return;
    }

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
            invalidateHomeScreen(false);
        }
        if (!drawn || labelChanged || millis() - lastDrawMs >= 250) {
            tft.setTextFont(1);
            tft.setTextSize(1);
            tft.setTextDatum(TC_DATUM);
            int w = tft.textWidth(label);
            // Clear rect + the 8px text cell both end at row 237: rows 238-239
            // belong to the bottom quadrants' market progress bars.
            int cx = screenWidth / 2;
            int y = screenHeight - 10;
            tft.fillRect(cx - w / 2 - 2, y, w + 4, 8, clockBackgroundColor);
            tft.setTextColor(captive ? TFT_ORANGE : TFT_RED, clockBackgroundColor);
            tft.drawString(label, cx, y);
            drawn = true;
            shownLabel = label;
            lastDrawMs = millis();
        }
    } else if (drawn) {
        drawn = false;
        shownLabel = nullptr;
        // Full repaint to restore what the label covered.
        invalidateHomeScreen(false);
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

    // Advance the stopwatch/countdown engines every loop, whatever page is
    // showing - the timers keep counting while settings/status are open.
    timersService();

    // Final countdown alarm: flashes over any screen until acknowledged; the
    // acknowledging tap is consumed and the UI returns to the countdown face.
    if (timerAlarmService())
    {
        resetFlashChangeFlag();
        return;
    }

    // Milestone reminder banner (non-blocking, ~1.6s) on the current screen.
    timerOverlayService();

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

    // Without a stored calibration, touches land off-target (the library's
    // default mapping); offer the calibration screen once per boot. It times
    // out back to the clock after a minute if nobody is around, and stays
    // reachable later via the CALTOUCH serial command or
    // /api/screen?name=caltouch.
    static bool touchCalOffered = false;
    if (!touchCalOffered && !touchCalibrationAvailable())
    {
        touchCalOffered = true;
        openTouchCalibration();
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
