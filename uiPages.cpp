#include "uiPages.h"

#include <SPIFFS.h>       // filesystem usage row on the status page
#include <WiFi.h>
#include <esp_system.h>   // esp_reset_reason - reset reason row
#include <soc/soc_caps.h> // SOC_TEMP_SENSOR_SUPPORTED - CPU temp row

#include "brightness.h"
#include "clockFaces.h"         // ClockFace enum, clockFaceName
#include "deviceIdentity.h"     // startup device label + MAC
#include "firmwareInfo.h"       // firmwareGitHash
#include "genericBaseProject.h" // BACKLIGHT_PIN, NTP sync state
#include "holidayService.h"     // holidaysInvalidate, holidayZonesLoaded
#include "marketHolidays.h"     // marketHolidaysFetchedInfo - status page
#include "projectConfig.h"
#include "weatherService.h"     // weatherInvalidate
#include "wifiWatch.h"          // outage history rows on the status page
#include "netCheck.h"           // captivePortalActive - Wi-Fi login helper
#include "wifiRelay.h"          // captive-portal login relay

UIScreen uiScreen = SCREEN_HOME;
bool uiPageDrawn = false;      // false -> render the full page on the next loop
int zoneSlotBeingEdited = 0;   // which quadrant the timezone list is editing
int tzListPage = 0;            // current page of the timezone list
unsigned long lastStatusRefresh = 0;

// After a screen switch, ignore the touch panel until the finger is lifted so
// a single tap can't "click through" onto the newly drawn page.
bool touchSuppressedUntilRelease = false;

/*-------- Timezone presets ----------*/

// The posix column carries each zone's current POSIX TZ rules (matching the
// tz database's POSIX representation, including DST transitions). They are
// only used as a fallback when the timezone server is unreachable AND no
// cached definition exists, so the clock still shows correct local time on a
// first boot without the server. Update an entry if a region changes its
// DST law (rare).
//
// The country column feeds the public-holiday service (date.nager.at). It is
// "" for Dubai and Mumbai because that API has no AE / IN calendars - those
// zones simply show no holidays.
const TimezonePreset TZ_PRESETS[] = {
    {"SANTA CLARA", "America/Los_Angeles", 37.35, -121.95, "PST8PDT,M3.2.0,M11.1.0", "US"},
    {"DENVER", "America/Denver", 39.74, -104.99, "MST7MDT,M3.2.0,M11.1.0", "US"},
    {"CHICAGO", "America/Chicago", 41.88, -87.63, "CST6CDT,M3.2.0,M11.1.0", "US"},
    {"NEW YORK", "America/New_York", 40.71, -74.01, "EST5EDT,M3.2.0,M11.1.0", "US"},
    {"SAO PAULO", "America/Sao_Paulo", -23.55, -46.63, "<-03>3", "BR"},
    {"LONDON", "Europe/London", 51.51, -0.13, "GMT0BST,M3.5.0/1,M10.5.0", "GB"},
    {"PARIS", "Europe/Paris", 48.86, 2.35, "CET-1CEST,M3.5.0,M10.5.0/3", "FR"},
    {"BERLIN", "Europe/Berlin", 52.52, 13.41, "CET-1CEST,M3.5.0,M10.5.0/3", "DE"},
    {"MOSCOW", "Europe/Moscow", 55.76, 37.62, "MSK-3", "RU"},
    {"DUBAI", "Asia/Dubai", 25.20, 55.27, "<+04>-4", ""},
    {"MUMBAI", "Asia/Kolkata", 19.08, 72.88, "IST-5:30", ""},
    {"SINGAPORE", "Asia/Singapore", 1.35, 103.82, "<+08>-8", "SG"},
    {"HONG KONG", "Asia/Hong_Kong", 22.32, 114.17, "HKT-8", "HK"},
    {"BEIJING", "Asia/Shanghai", 39.90, 116.41, "CST-8", "CN"},
    {"TOKYO", "Asia/Tokyo", 35.68, 139.69, "JST-9", "JP"},
    {"SEOUL", "Asia/Seoul", 37.57, 126.98, "KST-9", "KR"},
    {"SYDNEY", "Australia/Sydney", -33.87, 151.21, "AEST-10AEDT,M10.1.0,M4.1.0/3", "AU"},
    {"AUCKLAND", "Pacific/Auckland", -36.85, 174.76, "NZST-12NZDT,M9.5.0,M4.1.0/3", "NZ"},
};
const int TZ_PRESET_COUNT = sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]);
const int TZ_PER_PAGE = 5;

const char *getPosixFallback(const String &tz)
{
    for (int i = 0; i < TZ_PRESET_COUNT; i++)
    {
        if (tz == TZ_PRESETS[i].tz)
        {
            return TZ_PRESETS[i].posix;
        }
    }
    return nullptr;
}

const char *getCountryForTimezone(const String &tz)
{
    for (int i = 0; i < TZ_PRESET_COUNT; i++)
    {
        if (tz == TZ_PRESETS[i].tz)
        {
            return TZ_PRESETS[i].country;
        }
    }
    return nullptr;
}

bool getCityCoords(const String &tz, float &lat, float &lon)
{
    for (int i = 0; i < TZ_PRESET_COUNT; i++)
    {
        if (tz == TZ_PRESETS[i].tz)
        {
            lat = TZ_PRESETS[i].lat;
            lon = TZ_PRESETS[i].lon;
            return true;
        }
    }
    return false;
}

MarketInfo getMarketInfoForTimezone(const String &tz)
{
    if (tz == "America/New_York")
    {
        return {"NYSE", true, {
            {9, 30, 16, 0, "REGULAR"},
            {16, 0, 20, 0, "AFTER-HRS"},
            {20, 0, 4, 0, "OVERNIGHT"},
            {4, 0, 9, 30, "PRE-MARKET"}
        }, 4};
    }
    if (tz == "Asia/Shanghai")
    {
        return {"SSE", true, {
            {9, 0, 9, 30, "PRE-MARKET"},
            {9, 30, 11, 30, "REGULAR"},
            {13, 0, 15, 0, "REGULAR"},
            {15, 0, 15, 30, "AFTER-HRS"}
        }, 4};
    }
    if (tz == "Europe/London")
    {
        return {"LSE", true, {
            {7, 15, 8, 0, "PRE-MARKET"},
            {8, 0, 16, 30, "REGULAR"},
            {16, 30, 17, 0, "CLOSING"},
            {17, 0, 17, 30, "AFTER-HRS"}
        }, 4};
    }
    if (tz == "Asia/Tokyo")
    {
        return {"TSE", true, {
            {9, 0, 11, 30, "REGULAR"},
            {12, 30, 15, 30, "REGULAR"}
        }, 2};
    }
    if (tz == "Asia/Hong_Kong")
    {
        return {"HKEX", true, {
            {9, 30, 12, 0, "REGULAR"},
            {13, 0, 16, 0, "REGULAR"}
        }, 2};
    }
    return {"", false, {}, 0};
}

void applyConfiguredZones()
{
    for (int i = 0; i < 4; i++)
    {
        if (projectConfig.zoneTZ[i].length() == 0)
            continue;
        worldZones[i].name = projectConfig.zoneName[i];
        worldZones[i].timezone = projectConfig.zoneTZ[i];
        worldZones[i].market = getMarketInfoForTimezone(projectConfig.zoneTZ[i]);
    }
}

static bool zoneRulesMatch(WorldClockZone &zone, const String &timezone)
{
    return zone.tz.getOlson().equalsIgnoreCase(timezone);
}

/*-------- Buttons ----------*/

struct UIButton
{
    int x, y, w, h;
};

bool buttonContains(const UIButton &b, int tx, int ty)
{
    return tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h;
}

static int scaleUiX(int value)
{
    return (value * screenWidth + 160) / 320;
}

static int scaleUiY(int value)
{
    return (value * screenHeight + 120) / 240;
}

static UIButton scaleUiButton(const UIButton &base)
{
    UIButton b = {
        scaleUiX(base.x),
        scaleUiY(base.y),
        scaleUiX(base.w),
        scaleUiY(base.h)
    };
    if (b.x + b.w > screenWidth)
        b.w = screenWidth - b.x;
    if (b.y + b.h > screenHeight)
        b.h = screenHeight - b.y;
    return b;
}

static int uiTextColsFromX(int x)
{
    int px = screenWidth - x - scaleUiX(4);
    return max(1, px / 6);
}

void drawButton(const UIButton &b, const String &label, uint16_t border, uint16_t textColor)
{
    tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, border);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(textColor, clockBackgroundColor);
    tft.drawString(label, b.x + b.w / 2, b.y + b.h / 2);
}

// Settings page layout (7 rows, 30px pitch, below the 26px title)
const UIButton BTN_SET_TZ = {20, 28, 280, 26};
const UIButton BTN_SET_FACE = {20, 58, 280, 26};
const UIButton BTN_SET_CLK = {20, 88, 280, 26};
const UIButton BTN_SET_DATE = {20, 118, 280, 26};
// Row 148 holds two toggles side by side: quadrant grid + weather alerts.
const UIButton BTN_SET_GRID = {20, 148, 135, 26};
const UIButton BTN_SET_WXALERT = {165, 148, 135, 26};
const UIButton BTN_SET_DIM = {20, 178, 60, 26};
const UIButton BTN_SET_BRI = {240, 178, 60, 26};
// Bottom row: status / logs / touch calibration / WiFi helper / back.
const UIButton BTN_SET_STAT = {8, 208, 54, 26};
const UIButton BTN_SET_LOGS = {66, 208, 42, 26};
const UIButton BTN_SET_TOUCH = {112, 208, 50, 26};
const UIButton BTN_SET_WIFI = {166, 208, 82, 26};
const UIButton BTN_SET_BACK = {252, 208, 60, 26};
const UIButton SETTINGS_BRIGHTNESS_LABEL = {85, 178, 150, 26};

static UIButton settingsButton(const UIButton &base)
{
    return scaleUiButton(base);
}

// Wi-Fi login helper page: a single "Done" button (mirrors BTN_SET_BACK).
const UIButton BTN_WIFI_DONE = {90, 202, 140, 32};

// Wi-Fi failure page: Reboot / Settings side by side.
const UIButton BTN_FAIL_REBOOT = {30, 192, 120, 34};
const UIButton BTN_FAIL_SET = {170, 192, 120, 34};

// Zone-pick page layout (2x2 grid mirroring the clock quadrants)
const UIButton BTN_ZONE[4] = {
    {10, 36, 145, 76},
    {165, 36, 145, 76},
    {10, 118, 145, 76},
    {165, 118, 145, 76}
};
const UIButton BTN_ZONE_BACK = {90, 202, 140, 32};
const char *SLOT_LABELS[4] = {"TOP-LEFT", "TOP-RIGHT", "BOTTOM-LEFT", "BOTTOM-RIGHT"};

// Timezone list page layout
UIButton tzRowButton(int row)
{
    UIButton b = {10, 34 + row * 32, 300, 28};
    return scaleUiButton(b);
}
const UIButton BTN_TZ_PREV = {10, 202, 90, 32};
const UIButton BTN_TZ_BACK = {115, 202, 90, 32};
const UIButton BTN_TZ_NEXT = {220, 202, 90, 32};

/*-------- Touch input ----------*/

// Edge-triggered touch: fires once per physical tap (press after a release).
bool uiNewTouch(int &tx, int &ty)
{
    static bool wasDown = false;
    static unsigned long lastFire = 0;

    TouchPoint t = readTouchPoint();
    bool down = (t.zRaw > 800);
    bool fired = false;

    if (!down)
    {
        touchSuppressedUntilRelease = false;
    }
    else if (!wasDown && !touchSuppressedUntilRelease && millis() - lastFire > 150)
    {
        tx = t.x;
        ty = t.y;
        fired = true;
        lastFire = millis();
    }
    wasDown = down;
    return fired;
}

/*-------- Screen switching ----------*/

void switchToScreen(UIScreen s)
{
    // Leaving the Wi-Fi login helper: always tear the helper AP + NAT back down
    // so the clock returns to normal STA operation, whatever the exit path.
    if (uiScreen == SCREEN_WIFI_LOGIN && s != SCREEN_WIFI_LOGIN)
    {
        wifiRelayStop();
    }

    // Leaving the touch calibration screen: restore the user's orientation
    // (the screen forces the board's normal rotation while sampling corners),
    // whatever the exit path.
    if (uiScreen == SCREEN_TOUCH_CAL && s != SCREEN_TOUCH_CAL)
    {
        tft.setRotation(projectConfig.flipDisplay ? BOARD_TFT_ROTATION_FLIPPED
                                                  : BOARD_TFT_ROTATION_NORMAL);
        // Repaint whichever page is next in the restored orientation
        tft.fillScreen(clockBackgroundColor);
    }

    uiScreen = s;
    uiPageDrawn = false;
    touchSuppressedUntilRelease = true; // don't click through onto the new page

    if (s == SCREEN_HOME)
    {
        // Force a full clock redraw when returning home
        tft.fillScreen(clockBackgroundColor);
        firstDraw = true;
        for (int i = 0; i < 4; i++)
        {
            worldZones[i].initialized = false;
        }
    }
}

void openWifiLoginHelper()
{
    wifiRelayStart(); // bring up the helper AP + NAT before showing the screen
    switchToScreen(SCREEN_WIFI_LOGIN);
}

/*-------- Touch calibration screen ----------*/
// Both supported boards use an XPT2046 but through different drivers. This
// non-blocking wizard samples raw readings on either path, keeping the web
// server responsive while the user taps the four corner arrows.

// Distinguishes CYD xMin/xMax/yMin/yMax data from TFT_eSPI's fifth flags word.
static const uint16_t TOUCH_CAL_BITBANG_MARKER = 0x8001;

bool touchCalibrationAvailable()
{
    if (!projectConfig.touchCalSet) return false;
#if BOARD_TOUCH_DRIVER == BOARD_TOUCH_DRIVER_BITBANG
    return projectConfig.touchCal[4] == TOUCH_CAL_BITBANG_MARKER;
#else
    return projectConfig.touchCal[4] <= 7; // rotate/invert flags only
#endif
}

static const int TCAL_ARROW = 24;                       // corner arrow size
static const int TCAL_SAMPLES = 8;                      // raw samples per corner
static const uint16_t TCAL_Z_MIN = 200;                 // Z_THRESHOLD/2, corners read weak
static const uint16_t TCAL_RAW_ERR = 20;                // deadband between paired reads
static const unsigned long TCAL_TIMEOUT_MS = 60000;     // per-corner give-up

// Corner order matches TFT_eSPI::calibrateTouch so its mapping math can be
// reused verbatim: 0 = top-left, 1 = bottom-left, 2 = top-right,
// 3 = bottom-right. touchCalRaw holds the averaged raw x,y per corner.
static int touchCalCorner;
static int touchCalSampleCount;
static int32_t touchCalRaw[8];
static bool touchCalWaitRelease;
static unsigned long touchCalCornerStart;

static void drawTouchCalArrow(int corner, uint16_t color)
{
    int s = TCAL_ARROW;
    int w = screenWidth, h = screenHeight;
    switch (corner)
    {
    case 0: // top-left
        tft.drawLine(0, 0, 0, s, color);
        tft.drawLine(0, 0, s, 0, color);
        tft.drawLine(0, 0, s, s, color);
        break;
    case 1: // bottom-left
        tft.drawLine(0, h - s - 1, 0, h - 1, color);
        tft.drawLine(0, h - 1, s, h - 1, color);
        tft.drawLine(0, h - 1, s, h - s - 1, color);
        break;
    case 2: // top-right
        tft.drawLine(w - s - 1, 0, w - 1, 0, color);
        tft.drawLine(w - 1, 0, w - 1, s, color);
        tft.drawLine(w - 1, 0, w - s - 1, s, color);
        break;
    case 3: // bottom-right
        tft.drawLine(w - s - 1, h - 1, w - 1, h - 1, color);
        tft.drawLine(w - 1, h - 1 - s, w - 1, h - 1, color);
        tft.drawLine(w - 1, h - 1, w - s - 1, h - s - 1, color);
        break;
    }
}

static void drawTouchCalProgress()
{
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_YELLOW, clockBackgroundColor);
    tft.drawString("Corner " + String(touchCalCorner + 1) + " of 4  ",
                   screenWidth / 2, screenHeight / 2 + scaleUiY(30));
}

static void renderTouchCalPage()
{
    tft.fillScreen(clockBackgroundColor);
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("TOUCH CALIBRATION", screenWidth / 2,
                   screenHeight / 2 - scaleUiY(24));
    tft.setTextFont(2);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString("Tap the tip of each green arrow",
                   screenWidth / 2, screenHeight / 2);
    tft.drawString("Untouched, the clock returns by itself",
                   screenWidth / 2, screenHeight / 2 + scaleUiY(14));
    drawTouchCalProgress();
    drawTouchCalArrow(touchCalCorner, TFT_GREEN);
}

static void closeTouchCalibration()
{
    // switchToScreen restores the user's orientation when leaving this screen
    switchToScreen(SCREEN_HOME);
}

// Port of the tail of TFT_eSPI::calibrateTouch: derive the axis swap, the
// per-axis inversion and the raw ranges from the four corner readings.
static void finishTouchCalibration()
{
    const int32_t *v = touchCalRaw;
#if BOARD_TOUCH_DRIVER == BOARD_TOUCH_DRIVER_TFT_ESPI
    int32_t x0, x1, y0, y1;
    bool rotate = abs(v[0] - v[2]) > abs(v[1] - v[3]);
    if (rotate)
    {
        x0 = (v[1] + v[3]) / 2; // raw y tracks screen x
        x1 = (v[5] + v[7]) / 2;
        y0 = (v[0] + v[4]) / 2;
        y1 = (v[2] + v[6]) / 2;
    }
    else
    {
        x0 = (v[0] + v[2]) / 2;
        x1 = (v[4] + v[6]) / 2;
        y0 = (v[1] + v[5]) / 2;
        y1 = (v[3] + v[7]) / 2;
    }
    bool invertX = x0 > x1;
    if (invertX)
    {
        int32_t t = x0; x0 = x1; x1 = t;
    }
    bool invertY = y0 > y1;
    if (invertY)
    {
        int32_t t = y0; y0 = y1; y1 = t;
    }
    x1 -= x0;
    y1 -= y0;
    if (x0 == 0) x0 = 1;
    if (x1 == 0) x1 = 1;
    if (y0 == 0) y0 = 1;
    if (y1 == 0) y1 = 1;

    projectConfig.touchCal[0] = (uint16_t)x0;
    projectConfig.touchCal[1] = (uint16_t)x1;
    projectConfig.touchCal[2] = (uint16_t)y0;
    projectConfig.touchCal[3] = (uint16_t)y1;
    projectConfig.touchCal[4] = (uint16_t)(rotate | (invertX << 1) | (invertY << 2));
#else
    // The bitbang driver maps the raw axes directly. Preserve reversed ranges
    // (Arduino map supports them) so panel variants with an inverted axis do
    // not need separate orientation flags.
    int32_t left = (v[0] + v[2]) / 2;
    int32_t right = (v[4] + v[6]) / 2;
    int32_t top = (v[1] + v[5]) / 2;
    int32_t bottom = (v[3] + v[7]) / 2;
    if (abs(left - right) < 100 || abs(top - bottom) < 100)
    {
        Log.println("Touch calibration rejected - corner readings are too close");
        closeTouchCalibration();
        return;
    }
    projectConfig.touchCal[0] = (uint16_t)left;
    projectConfig.touchCal[1] = (uint16_t)right;
    projectConfig.touchCal[2] = (uint16_t)top;
    projectConfig.touchCal[3] = (uint16_t)bottom;
    projectConfig.touchCal[4] = TOUCH_CAL_BITBANG_MARKER;
#endif
    projectConfig.touchCalSet = true;
    projectConfig.saveConfigFile();
#if BOARD_TOUCH_DRIVER == BOARD_TOUCH_DRIVER_TFT_ESPI
    tft.setTouch(projectConfig.touchCal);
#else
    touchscreen.setCalibration(projectConfig.touchCal[0], projectConfig.touchCal[1],
                               projectConfig.touchCal[2], projectConfig.touchCal[3]);
#endif

    Log.print("Touch calibration saved: ");
    for (int i = 0; i < 5; i++)
    {
        Log.print(projectConfig.touchCal[i]);
        Log.print(i < 4 ? ", " : "\n");
    }
    closeTouchCalibration();
}

// Called every loop while SCREEN_TOUCH_CAL is showing (renderUiPage).
static void serviceTouchCalScreen()
{
    if (millis() - touchCalCornerStart > TCAL_TIMEOUT_MS)
    {
        Log.println("Touch calibration timed out - keeping the previous mapping");
        closeTouchCalibration();
        return;
    }

    uint16_t xa, ya, xb, yb;
#if BOARD_TOUCH_DRIVER == BOARD_TOUCH_DRIVER_TFT_ESPI
    uint16_t za = tft.getTouchRawZ();
#else
    TouchPoint first = touchscreen.getTouch();
    uint16_t za = first.zRaw;
#endif
    if (za < TCAL_Z_MIN)
    {
        touchCalWaitRelease = false;
        return;
    }
    if (touchCalWaitRelease)
        return;

    // Two reads a moment apart must agree (the library's validTouch
    // deadband) so a finger sliding onto the corner doesn't skew the average.
#if BOARD_TOUCH_DRIVER == BOARD_TOUCH_DRIVER_TFT_ESPI
    tft.getTouchRaw(&xa, &ya);
    delay(2);
    if (tft.getTouchRawZ() < TCAL_Z_MIN)
        return;
    tft.getTouchRaw(&xb, &yb);
#else
    xa = first.xRaw;
    ya = first.yRaw;
    delay(2);
    TouchPoint second = touchscreen.getTouch();
    if (second.zRaw < TCAL_Z_MIN)
        return;
    xb = second.xRaw;
    yb = second.yRaw;
#endif
    if (abs((int)xa - (int)xb) > TCAL_RAW_ERR ||
        abs((int)ya - (int)yb) > TCAL_RAW_ERR)
        return;

    touchCalRaw[touchCalCorner * 2] += xa;
    touchCalRaw[touchCalCorner * 2 + 1] += ya;
    if (++touchCalSampleCount < TCAL_SAMPLES)
        return;

    touchCalRaw[touchCalCorner * 2] /= TCAL_SAMPLES;
    touchCalRaw[touchCalCorner * 2 + 1] /= TCAL_SAMPLES;

    if (++touchCalCorner >= 4)
    {
        finishTouchCalibration();
        return;
    }
    drawTouchCalArrow(touchCalCorner - 1, clockBackgroundColor);
    drawTouchCalArrow(touchCalCorner, TFT_GREEN);
    drawTouchCalProgress();
    touchCalSampleCount = 0;
    touchCalWaitRelease = true;
    touchCalCornerStart = millis();
}

void openTouchCalibration()
{
    // Corner raw values only line up with pixels in the board's normal
    // rotation - readTouchPoint() mirrors flipped displays on top of the
    // stored mapping, so calibrating under the flipped rotation would flip
    // touches twice. Flipped mounts see this one screen upside down.
    tft.setRotation(BOARD_TFT_ROTATION_NORMAL);
    touchCalCorner = 0;
    touchCalSampleCount = 0;
    for (int i = 0; i < 8; i++)
    {
        touchCalRaw[i] = 0;
    }
    touchCalWaitRelease = true;
    touchCalCornerStart = millis();
    switchToScreen(SCREEN_TOUCH_CAL);
}

/*-------- Boot-time settings button + boot console ----------*/
// See uiPages.h: a Settings button on the "System initializing..." screen so
// the blocking boot waits can be cut short on networks with no usable
// internet (e.g. login-required WiFi), landing the main loop directly on the
// settings page.
//
// The rest of that screen is a boot console: it mirrors the newest log lines
// (WiFi attempts with SSID and failure reason, portal, NTP, timezone setup)
// so the boot never looks frozen and failures are diagnosable on the device.

const UIButton BTN_BOOT_SETTINGS = {90, 192, 140, 32};

static bool bootUiActive = false;       // button drawn, polling enabled
static bool bootSettingsWanted = false; // sticky once the button is tapped

// Console geometry: font 1 (6x8 px) rows between the title rule (y=38, drawn
// in displaySetup below the title + version line) and the Settings button
// (y=192).
static const int BOOT_CON_TOP = 42;
static const int BOOT_CON_LINE_H = 8;
static const int BOOT_CON_ROWS = 18;

static String bootConShown[BOOT_CON_ROWS]; // what each row currently displays
static uint32_t bootConLastBytes = 0;
static unsigned long bootConLastDraw = 0;

static int bootConsoleTop()
{
    return scaleUiY(BOOT_CON_TOP);
}

static int bootConsoleLineH()
{
    return max(BOOT_CON_LINE_H, scaleUiY(BOOT_CON_LINE_H));
}

static int bootConsoleCols()
{
    return uiTextColsFromX(scaleUiX(2));
}

// Mirror the tail of the log ring into the console area. Rows are cached and
// only repainted when their text changes, so the frequent polls stay cheap
// and the screen doesn't flicker. force repaints everything (after another
// page wiped the screen).
static void bootConsoleRender(bool force)
{
    if (!bootUiActive)
        return;
    uint32_t bytes = logBytesWritten();
    if (!force)
    {
        if (bytes == bootConLastBytes)
            return;
        // Throttle repaints; the 50ms boot polls pick the change up shortly.
        if (millis() - bootConLastDraw < 100)
            return;
    }
    bootConLastBytes = bytes;
    bootConLastDraw = millis();

    // Generous byte budget so even long (later-truncated) lines still leave a
    // full screen of rows - logTail starts at a line boundary and drops a
    // clipped first line itself.
    int cols = bootConsoleCols();
    String tail = logTail(BOOT_CON_ROWS * (cols + 48));
    // A trailing newline would show as a phantom empty last line; without it
    // the last line is the newest (possibly still-forming) one.
    if (tail.endsWith("\n"))
        tail.remove(tail.length() - 1);

    // Keep the last BOOT_CON_ROWS lines (ring over rows[], oldest overwritten).
    String rows[BOOT_CON_ROWS];
    int total = 0;
    if (tail.length() > 0)
    {
        int start = 0;
        while (start <= (int)tail.length())
        {
            int nl = tail.indexOf('\n', start);
            int end = (nl < 0) ? tail.length() : nl;
            rows[total % BOOT_CON_ROWS] = tail.substring(start, end);
            total++;
            if (nl < 0)
                break;
            start = nl + 1;
        }
    }

    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_SILVER, clockBackgroundColor);
    for (int i = 0; i < BOOT_CON_ROWS; i++)
    {
        int idx = total - BOOT_CON_ROWS + i; // log line shown on row i
        String line = (idx >= 0) ? rows[idx % BOOT_CON_ROWS] : String();
        if (line.length() > cols)
            line = line.substring(0, cols);
        if (!force && line == bootConShown[i])
            continue;
        bootConShown[i] = line;
        int y = bootConsoleTop() + i * bootConsoleLineH();
        tft.fillRect(0, y, screenWidth, bootConsoleLineH(), clockBackgroundColor);
        tft.drawString(line, scaleUiX(2), y);
    }
}

static void bootUiDrawChrome()
{
    bool tapped = bootSettingsWanted;
    drawButton(scaleUiButton(BTN_BOOT_SETTINGS), "Settings",
               tapped ? TFT_GREEN : TFT_CYAN, tapped ? TFT_GREEN : TFT_WHITE);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString("WiFi login / status / logs", screenWidth / 2,
                   screenHeight - scaleUiY(10));
}

void bootUiBegin()
{
#if BOARD_TOUCH_DRIVER == BOARD_TOUCH_DRIVER_BITBANG
    // The touch controller normally starts later (rollingClockSetup), but
    // begin() only sets pin modes, so starting it early here is harmless.
    // On shared-SPI boards this would steal the LCD's SPI pins, so those use
    // TFT_eSPI touch and need no separate begin call.
    touchscreen.begin();
#endif

    bootUiActive = true;
    bootUiDrawChrome();
    bootConsoleRender(true); // show the lines logged before the display came up
}

void bootUiRefresh()
{
    if (!bootUiActive)
        return;
    // Another page (conf-mode screen, SetupCYD) painted over the boot screen:
    // rebuild it wholesale, title included.
    tft.fillScreen(clockBackgroundColor);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("System initializing...", screenWidth / 2, scaleUiY(2));
    // Device identity under the title - see displaySetup.
    tft.setTextFont(1);
    tft.setTextColor(TFT_CYAN, clockBackgroundColor);
    tft.drawString(deviceLabel(), screenWidth / 2, scaleUiY(18));
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString(deviceMacAddress(), screenWidth / 2, scaleUiY(28));
    tft.drawFastHLine(0, scaleUiY(38), screenWidth, TFT_DARKGREY);
    bootUiDrawChrome();
    bootConsoleRender(true);
}

void bootUiEnd()
{
    bootUiActive = false;
}

bool bootUiSettingsRequested()
{
    return bootSettingsWanted;
}

bool bootUiPoll()
{
    if (!bootUiActive)
        return bootSettingsWanted;

    bootConsoleRender(false); // mirror any new log lines onto the screen

    if (bootSettingsWanted)
        return true;

    TouchPoint t = readTouchPoint();
    if (t.zRaw > 800 && buttonContains(scaleUiButton(BTN_BOOT_SETTINGS), t.x, t.y))
    {
        bootSettingsWanted = true;
        // Acknowledge right away - the remaining boot steps can still take a
        // few seconds before the settings page actually appears.
        drawButton(scaleUiButton(BTN_BOOT_SETTINGS), "Settings", TFT_GREEN, TFT_GREEN);
        Log.println("Settings requested from the init screen - cutting the "
                    "remaining boot waits short");
        bootConsoleRender(true);
    }
    return bootSettingsWanted;
}

/*-------- Boot-time Wi-Fi failure page ----------*/
// See uiPages.h: when credentials just entered in the config portal fail to
// join, the boot path records the details here and the main loop shows
// SCREEN_WIFI_FAIL instead of the old silent reboot loop.

static String wifiFailSsid;
static int wifiFailStatus = 0;
static bool wifiFailPending = false;
static bool wifiFailPreview = false; // opened via /api/screen with demo data
static unsigned long wifiFailShownAt = 0;

// Auto-reboot the failure page after this long untouched, so an unattended
// clock still runs the boot recovery sequence (portal included) on its own.
static const unsigned long WIFI_FAIL_AUTO_REBOOT_MS = 5UL * 60UL * 1000UL;

void bootReportWifiFailure(const String &ssid, int wlStatus)
{
    wifiFailSsid = ssid;
    wifiFailStatus = wlStatus;
    wifiFailPreview = false;
    wifiFailPending = true;
    // Cut the remaining boot network waits short, same as a Settings tap on
    // the init screen; bootOpenPendingScreen then opens the failure page.
    bootSettingsWanted = true;
}

void bootOpenPendingScreen()
{
    // Boot is over: the main loop owns the screen from here (clock or one of
    // the pages below); stop the console/button so they can't paint over it.
    bootUiEnd();

    if (wifiFailPending)
    {
        wifiFailPending = false;
        switchToScreen(SCREEN_WIFI_FAIL);
    }
    else if (bootSettingsWanted)
    {
        switchToScreen(SCREEN_SETTINGS);
    }
}

// One-line diagnosis + one-line detail for a wl_status_t join result.
static void wifiFailReason(int st, const char *&problem, const char *&detail)
{
    switch (st)
    {
    case WL_NO_SSID_AVAIL:
        problem = "network not found";
        detail = "Out of range, hidden, or name typo.";
        break;
    case WL_CONNECT_FAILED:
        problem = "join rejected";
        detail = "Almost always a wrong password.";
        break;
    case WL_CONNECTION_LOST:
        problem = "connection lost mid-join";
        detail = "Weak signal? Move nearer the router.";
        break;
    default:
        problem = "no response";
        detail = "Bad password, weak signal or filter.";
        break;
    }
}

void renderWifiFailPage()
{
    tft.fillScreen(clockBackgroundColor);

    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_RED, clockBackgroundColor);
    tft.drawString("WIFI CONNECT FAILED", screenWidth / 2, scaleUiY(2));

    const char *problem, *detail;
    wifiFailReason(wifiFailStatus, problem, detail);

    String ssid = wifiFailSsid.length() ? wifiFailSsid : "(no network name)";
    if (ssid.length() > 24) ssid = ssid.substring(0, 24);

    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString("Network:", scaleUiX(12), scaleUiY(36));
    tft.setTextColor(TFT_YELLOW, clockBackgroundColor);
    tft.drawString(ssid, scaleUiX(90), scaleUiY(36));

    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString("Problem:", scaleUiX(12), scaleUiY(56));
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString(String(problem) + " (" + String(wifiFailStatus) + ")",
                   scaleUiX(90), scaleUiY(56));
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString(detail, scaleUiX(12), scaleUiY(76));

    tft.drawString("Reboot: try again now (the setup", scaleUiX(12), scaleUiY(104));
    tft.drawString("portal reopens if it fails again).", scaleUiX(12), scaleUiY(120));
    tft.drawString("Settings: open status & logs; WiFi", scaleUiX(12), scaleUiY(144));
    tft.drawString("keeps retrying in the background.", scaleUiX(12), scaleUiY(160));

    drawButton(scaleUiButton(BTN_FAIL_REBOOT), "Reboot", TFT_RED, TFT_WHITE);
    drawButton(scaleUiButton(BTN_FAIL_SET), "Settings", TFT_CYAN, TFT_WHITE);

    tft.setTextFont(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    if (wifiFailPreview)
    {
        // Seeded demo details, opened via /api/screen: make sure a captured
        // screenshot can't be mistaken for a real outage, and don't let the
        // unattended-recovery reboot fire under a developer's feet.
        tft.setTextColor(TFT_ORANGE, clockBackgroundColor);
        tft.drawString("PREVIEW - demo data, auto-reboot off", screenWidth / 2, scaleUiY(232));
    }
    else
    {
        tft.drawString("reboots by itself after 5 minutes", screenWidth / 2, scaleUiY(232));
    }

    wifiFailShownAt = millis();
}

/*-------- Settings actions ----------*/

void saveDisplayPrefs()
{
    projectConfig.twentyFourHour = SHOW_24HOUR;
    projectConfig.usDateFormat = !NOT_US_DATE;
    projectConfig.saveConfigFile();
}

void drawSettingsBrightnessLabel()
{
    UIButton b = settingsButton(SETTINGS_BRIGHTNESS_LABEL);
    tft.fillRect(b.x, b.y, b.w, b.h, clockBackgroundColor);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    int pct = brightnessPercent(backlightLevel);
    tft.drawString("Brightness " + String(pct) + "%", b.x + b.w / 2, b.y + b.h / 2);
}

void adjustBacklightFromUi(int delta)
{
    backlightLevel = clampBrightness(backlightLevel + delta);
    analogWrite(BACKLIGHT_PIN, backlightLevel);
    // Hold this manual setting before auto-brightness resumes
    manualBrightnessUntil = millis() + MANUAL_BRIGHTNESS_HOLD_MS;
    // Persist so the level survives a reboot (taps are discrete, so this
    // stays well within SPIFFS write-endurance territory)
    projectConfig.brightness = backlightLevel;
    projectConfig.saveConfigFile();
    drawSettingsBrightnessLabel();
    Log.println("Brightness set from settings page: " + String(backlightLevel));
}

// Apply a timezone preset to a quadrant, persist it, and re-fetch the zone
// definition from the ezTime server (brief blocking network call). Declared
// in uiPages.h - also used by the web settings page.
void applyZoneSelection(int slot, const TimezonePreset &preset)
{
    tft.fillScreen(clockBackgroundColor);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, clockBackgroundColor);
    tft.drawString("Loading " + String(preset.name) + "...",
                   screenWidth / 2, screenHeight / 2);

    worldZones[slot].name = preset.name;
    worldZones[slot].timezone = preset.tz;
    worldZones[slot].market = getMarketInfoForTimezone(preset.tz);
    worldZones[slot].lastMarketStatus = "";
    worldZones[slot].lastHour = -1;
    worldZones[slot].lastMinute = -1;
    worldZones[slot].lastDay = -1;
    worldZones[slot].initialized = false;
    worldZones[slot].weatherAlert = "";
    worldZones[slot].weatherNotice = "";

    // Start from a clean ezTime object when a slot changes. Reusing the old
    // object can leave the previous city's rules alive if a rapid sequence of
    // timezone-server lookups gets a stale UDP response.
    worldZones[slot].tz = Timezone();

    bool tzReady = false;
    if (worldZones[slot].tz.setCache(slot * EEPROM_CACHE_LEN) &&
        zoneRulesMatch(worldZones[slot], preset.tz))
    {
        tzReady = true;
    }

    if (!tzReady)
    {
        bool fetched = worldZones[slot].tz.setLocation(preset.tz);
        if (!(fetched && zoneRulesMatch(worldZones[slot], preset.tz)))
        {
            String got = fetched ? worldZones[slot].tz.getOlson() : String("");
            if (got.length() > 0)
            {
                Log.println("Timezone lookup for " + String(preset.tz) +
                            " returned " + got + " - using built-in POSIX rules");
            }
            else
            {
                Log.println("Failed to fetch timezone " + String(preset.tz) +
                            " - using built-in POSIX rules");
            }
            worldZones[slot].tz.setPosix(preset.posix);
        }
    }

    if (worldZones[slot].market.hasMarket)
    {
        worldZones[slot].lastMarketStatus = getMarketStatus(worldZones[slot]);
    }

    projectConfig.zoneName[slot] = preset.name;
    projectConfig.zoneTZ[slot] = preset.tz;
    projectConfig.saveConfigFile();

    // Cached weather / holidays are for the old city - refetch as needed
    weatherInvalidate();
    holidaysInvalidate();

    Log.println("Quadrant " + String(slot) + " set to " + String(preset.name) +
                   " (" + String(preset.tz) + ")");
}

/*-------- Page rendering ----------*/

void renderSettingsPage()
{
    tft.fillScreen(clockBackgroundColor);

    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("SETTINGS", scaleUiX(8), scaleUiY(2));

    // Firmware identity in the header's right gutter: the compile timestamp
    // (the "version") over the short git hash, both small so they stay clear
    // of the title and buttons.
    tft.setTextFont(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString(String(__DATE__) + " " + __TIME__, screenWidth - scaleUiX(4), scaleUiY(4));
    tft.drawString("git " + String(firmwareGitHash()), screenWidth - scaleUiX(4), scaleUiY(15));

    drawButton(settingsButton(BTN_SET_TZ), "Change timezones  >", TFT_CYAN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_FACE),
               "Clock face: " + String(clockFaceName(projectConfig.clockFace)),
               TFT_CYAN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_CLK),
               SHOW_24HOUR ? "Clock format: 24 hour" : "Clock format: 12 hour (AM/PM)",
               TFT_CYAN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_DATE),
               NOT_US_DATE ? "Date format: DD/MM/YY" : "Date format: MM/DD/YY",
               TFT_CYAN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_GRID),
               projectConfig.showGrid ? "Grid: On" : "Grid: Off",
               TFT_CYAN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_WXALERT),
               projectConfig.weatherAlerts ? "Wx alert: On" : "Wx alert: Off",
               TFT_CYAN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_DIM), "-", TFT_CYAN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_BRI), "+", TFT_CYAN, TFT_WHITE);
    drawSettingsBrightnessLabel();
    drawButton(settingsButton(BTN_SET_STAT), "Status", TFT_GREEN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_LOGS), "Logs", TFT_GREEN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_TOUCH), "Touch", TFT_CYAN, TFT_WHITE);
    // Enables the device's helper AP + captive-portal login relay. Amber when
    // a captive portal is blocking the internet, to draw the eye.
    drawButton(settingsButton(BTN_SET_WIFI), "WiFi login",
               captivePortalActive() ? TFT_ORANGE : TFT_CYAN, TFT_WHITE);
    drawButton(settingsButton(BTN_SET_BACK), "Back", TFT_DARKGREY, TFT_WHITE);
}

// The live status line of the Wi-Fi login helper (row 168), repainted once a
// second from renderUiPage as the relay state changes.
static void renderWifiLoginStatus()
{
    const char *text;
    uint16_t color;
    switch (wifiRelayState())
    {
    case RELAY_SUCCESS:
        text = "Online! You're connected.";
        color = TFT_GREEN;
        break;
    case RELAY_TIMEOUT:
        text = "Timed out - tap Done and retry.";
        color = TFT_RED;
        break;
    case RELAY_ACTIVE:
        // The relay only routes once the clock itself is associated to the
        // upstream network - say so while that link is still coming up.
        text = (WiFi.status() == WL_CONNECTED) ? "Waiting for login..."
                                               : "Waiting for WiFi link...";
        color = TFT_CYAN;
        break;
    default:
        text = "";
        color = clockBackgroundColor;
        break;
    }
    tft.fillRect(0, scaleUiY(166), screenWidth, scaleUiY(20), clockBackgroundColor);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color, clockBackgroundColor);
    tft.drawString(text, screenWidth / 2, scaleUiY(168));
}

// Full-screen helper for login-required networks: brings up a phone-joinable AP
// that NATs the login out through the clock's MAC (wifiRelay.cpp). The relay is
// serviced from renderUiPage; this only paints the current state.
void renderWifiLoginPage()
{
    tft.fillScreen(clockBackgroundColor);

    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("WI-FI LOGIN", screenWidth / 2, scaleUiY(2));

    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString("1. On your phone, join this Wi-Fi:", scaleUiX(12), scaleUiY(30));

    tft.setTextColor(TFT_YELLOW, clockBackgroundColor);
    tft.drawString(wifiRelayApSsid(), scaleUiX(24), scaleUiY(48));
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString(String("password: ") + wifiRelayApPassword(), scaleUiX(24), scaleUiY(64));

    tft.drawString("2. Sign in on the login page that", scaleUiX(12), scaleUiY(84));
    tft.drawString("   opens (none? try neverssl.com).", scaleUiX(12), scaleUiY(100));
    // Phones love to flee a hotspot that has "no internet" - that warning is
    // expected here and leaving the hotspot is the #1 way the login fails.
    tft.drawString("3. Stay on that Wi-Fi - ignore any", scaleUiX(12), scaleUiY(120));
    tft.drawString("   'no internet' warning, wait here.", scaleUiX(12), scaleUiY(136));

    renderWifiLoginStatus();

    drawButton(scaleUiButton(BTN_WIFI_DONE), "Done", TFT_DARKGREY, TFT_WHITE);
}

void renderZonePickPage()
{
    tft.fillScreen(clockBackgroundColor);

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("TAP A CLOCK TO CHANGE ITS TIMEZONE", screenWidth / 2, scaleUiY(10));

    for (int i = 0; i < 4; i++)
    {
        UIButton b = scaleUiButton(BTN_ZONE[i]);
        int cx = b.x + b.w / 2;
        tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, TFT_CYAN);

        tft.setTextFont(1);
        tft.setTextSize(1);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
        tft.drawString(SLOT_LABELS[i], cx, b.y + scaleUiY(8));

        tft.setTextFont(2);
        tft.setTextColor(TFT_YELLOW, clockBackgroundColor);
        tft.drawString(worldZones[i].name, cx, b.y + scaleUiY(26));

        tft.setTextFont(1);
        tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
        tft.drawString(worldZones[i].timezone, cx, b.y + scaleUiY(54));
    }

    drawButton(scaleUiButton(BTN_ZONE_BACK), "Back", TFT_DARKGREY, TFT_WHITE);
}

void renderTzListPage()
{
    int totalPages = (TZ_PRESET_COUNT + TZ_PER_PAGE - 1) / TZ_PER_PAGE;
    if (tzListPage < 0) tzListPage = 0;
    if (tzListPage >= totalPages) tzListPage = totalPages - 1;

    tft.fillScreen(clockBackgroundColor);

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString(String(SLOT_LABELS[zoneSlotBeingEdited]) + " CLOCK",
                   screenWidth / 2, scaleUiY(8));

    tft.setTextFont(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString(String(tzListPage + 1) + "/" + String(totalPages),
                   screenWidth - scaleUiX(6), scaleUiY(10));

    for (int row = 0; row < TZ_PER_PAGE; row++)
    {
        int idx = tzListPage * TZ_PER_PAGE + row;
        if (idx >= TZ_PRESET_COUNT)
            break;
        bool current = (worldZones[zoneSlotBeingEdited].timezone == TZ_PRESETS[idx].tz);
        drawButton(tzRowButton(row),
                   String(TZ_PRESETS[idx].name) + "  (" + TZ_PRESETS[idx].tz + ")",
                   current ? TFT_GREEN : TFT_DARKGREY,
                   current ? TFT_GREEN : TFT_WHITE);
    }

    drawButton(scaleUiButton(BTN_TZ_PREV), "< Prev", TFT_CYAN, TFT_WHITE);
    drawButton(scaleUiButton(BTN_TZ_BACK), "Back", TFT_DARKGREY, TFT_WHITE);
    drawButton(scaleUiButton(BTN_TZ_NEXT), "Next >", TFT_CYAN, TFT_WHITE);
}

/*-------- System status pages ----------*/
// Three pages, cycled by tapping the screen (the last tap returns to the
// settings page): 1) system, 2) network & storage, 3) clock data. The
// dynamic values refresh once a second while a page is showing.

const int STATUS_PAGE_COUNT = 3;
const int STATUS_MAX_ROWS = 11;
const int STATUS_VALUE_X = 96;
const int STATUS_ROW_Y0 = 34;
const int STATUS_ROW_STEP = 17;

int statusPageIndex = 0; // reset to 0 when entering from the settings page

static int statusValueX()
{
    return scaleUiX(STATUS_VALUE_X);
}

static int statusRowY(int row)
{
    return scaleUiY(STATUS_ROW_Y0 + row * STATUS_ROW_STEP);
}

static int statusRowH(int row)
{
    return max(STATUS_ROW_STEP, statusRowY(row + 1) - statusRowY(row));
}

static const char *STATUS_TITLES[STATUS_PAGE_COUNT] = {
    "SYSTEM STATUS", "NETWORK & STORAGE", "CLOCK DATA"};

// Row 3 is the CPU temperature where the chip has a usable sensor; on the
// classic ESP32 (no sensor) the row advertises the web settings UI instead
// of a permanent "no sensor on this chip".
static const char *STATUS_LABELS_SYSTEM[] = {
    "WiFi", "IP addr", "CPU",
#if SOC_TEMP_SENSOR_SUPPORTED
    "CPU temp",
#else
    "Web UI",
#endif
    "Flash", "Firmware",
    "Build", "Heap", "Uptime", "NTP sync", "UTC time"};
static const char *STATUS_LABELS_NETWORK[] = {
    "Hostname", "MAC", "Gateway", "DNS", "Channel", "Drops",
    "Reset", "SPIFFS", "Max alloc", "SDK"};
static const char *STATUS_LABELS_DATA[] = {
    "Home zone", "Face", "Format", "Weather", "Mkt hols", "Pub hols",
    "Backlight", "Auto-dim", "Hold", "Night"};

static int statusRowCount(int page)
{
    return page == 0 ? 11 : 10;
}

static const char *const *statusLabels(int page)
{
    switch (page)
    {
    case 1: return STATUS_LABELS_NETWORK;
    case 2: return STATUS_LABELS_DATA;
    default: return STATUS_LABELS_SYSTEM;
    }
}

const char *resetReasonText()
{
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software reset";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO reset";
    default: return "unknown";
    }
}

String formatUptime()
{
    unsigned long s = millis() / 1000;
    char buf[24];
    sprintf(buf, "%lud %02lu:%02lu:%02lu",
            s / 86400UL, (s / 3600UL) % 24UL, (s / 60UL) % 60UL, s % 60UL);
    return String(buf);
}

// Compact duration for the outage-history row: "45m", "3h", "2d".
static String agoText(unsigned long ms)
{
    unsigned long m = ms / 60000UL;
    if (m < 60) return String(m) + "m";
    if (m < 48 * 60) return String(m / 60) + "h";
    return String(m / (24 * 60)) + "d";
}

// Page 1: the classic system diagnostics.
static void fillSystemValues(String *values, uint16_t *colors)
{
    String ssid = WiFi.SSID();
    if (ssid.length() > 14) ssid = ssid.substring(0, 14);

    uint32_t sketch = ESP.getSketchSize();
    // getFreeSketchSpace() returns the size of the (equal-sized) OTA update
    // partition - i.e. the app slot's capacity, not "slot minus sketch" -
    // so it is the right denominator for the percentage on its own.
    uint32_t slot = ESP.getFreeSketchSpace();

    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    int rssi = WiFi.RSSI();

    values[0] = wifiUp ? ssid + " (" + String(rssi) + " dBm)" : "OFFLINE";
    colors[0] = !wifiUp        ? TFT_RED
                : (rssi > -60) ? TFT_GREEN
                : (rssi > -75) ? TFT_YELLOW
                               : TFT_RED;
    values[1] = WiFi.localIP().toString();
    values[2] = String(ESP.getChipModel()) + " r" + String(ESP.getChipRevision()) +
                " @" + String(ESP.getCpuFreqMHz()) + "MHz";
#if SOC_TEMP_SENSOR_SUPPORTED
    values[3] = String(temperatureRead(), 1) + " C";
#else
    values[3] = wifiUp ? "http://" + WiFi.localIP().toString() + "/"
                       : "(offline)";
#endif
    values[4] = String(ESP.getFlashChipSize() / (1024UL * 1024UL)) + " MB @ " +
                String(ESP.getFlashChipSpeed() / 1000000UL) + " MHz";
    values[5] = String(sketch / 1024) + " KB (" +
                String(slot > 0 ? (sketch * 100UL) / slot : 0) + "% of slot)";
    values[6] = String(__DATE__) + " " + __TIME__;
    values[7] = String(ESP.getFreeHeap() / 1024) + " KB (min " +
                String(ESP.getMinFreeHeap() / 1024) + ")";
    values[8] = formatUptime();
    values[9] = (syncCount > 0)
                    ? String(syncCount) + " (" +
                          String((millis() - lastSyncTime) / 60000UL) + " min ago)"
                    : "none (" + String(currentNtpServer()) + ")";
    values[10] = UTC.dateTime("H:i:s") + " UTC";
}

// Page 2: network details, outage history, reset reason and storage.
static void fillNetworkValues(String *values, uint16_t *colors)
{
    String host = projectConfig.hostname + ".local";
    if (host.length() > 28) host = host.substring(0, 28);
    values[0] = host;
    values[1] = WiFi.macAddress();
    if (projectConfig.staMacOverride.length() > 0)
    {
        values[1] += " *"; // "*" = a custom/cloned MAC is in use, not the factory one
        colors[1] = TFT_CYAN;
    }
    values[2] = WiFi.gatewayIP().toString();
    values[3] = WiFi.dnsIP().toString();
    values[4] = String(WiFi.channel());

    int drops = wifiDropCount();
    unsigned long offlineMs = wifiOfflineDurationMs();
    if (offlineMs > 0)
    {
        values[5] = String(drops) + " (offline " + agoText(offlineMs) + ")";
        colors[5] = TFT_RED;
    }
    else if (drops == 0)
    {
        values[5] = "none since boot";
    }
    else
    {
        values[5] = String(drops) + " (last " + agoText(wifiLastOutageDurationMs()) +
                    ", " + agoText(wifiLastOutageEndedAgoMs()) + " ago)";
        colors[5] = TFT_YELLOW;
    }

    esp_reset_reason_t rr = esp_reset_reason();
    values[6] = resetReasonText();
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT)
    {
        colors[6] = TFT_RED; // the previous run died abnormally
    }

    values[7] = String(SPIFFS.usedBytes() / 1024) + " / " +
                String(SPIFFS.totalBytes() / 1024) + " KB";
    values[8] = String(ESP.getMaxAllocHeap() / 1024) + " KB block";
    values[9] = ESP.getSdkVersion();
}

// Page 3: what the clock is showing and how fresh its background data is.
static void fillDataValues(String *values, uint16_t *colors)
{
    values[0] = worldZones[0].timezone;
    values[1] = clockFaceName(projectConfig.clockFace);
    values[2] = String(SHOW_24HOUR ? "24H" : "12H") + ", " +
                (NOT_US_DATE ? "DD/MM/YY" : "MM/DD/YY");

    long weatherAge = weatherAgeMinutes();
    if (weatherAge < 0)
    {
        values[3] = "no data yet";
        colors[3] = TFT_RED;
    }
    else
    {
        values[3] = "updated " + String(weatherAge) + " min ago";
        // Yellow once ~3 fetch intervals have gone by without fresh data
        if (weatherAge > 3 * constrain(projectConfig.weatherRefreshMin, 5, 120))
            colors[3] = TFT_YELLOW;
    }

    long calAgeDays = -1;
    if (marketHolidaysFetchedInfo(calAgeDays))
    {
        values[4] = calAgeDays < 0 ? "fetched calendars"
                                   : "fetched " + String(calAgeDays) + "d ago";
        colors[4] = TFT_GREEN;
    }
    else
    {
        values[4] = "compiled-in tables";
        colors[4] = TFT_YELLOW;
    }

    int eligible = 0;
    int loaded = holidayZonesLoaded(eligible);
    if (eligible == 0)
    {
        values[5] = "no eligible zones";
    }
    else
    {
        values[5] = String(loaded) + "/" + String(eligible) + " zones loaded";
        colors[5] = (loaded == eligible) ? TFT_GREEN : TFT_YELLOW;
    }

    values[6] = String(backlightLevel) + " (" + String(brightnessPercent(backlightLevel)) + "%)";

    bool ldrTrusted, ldrDark;
    int ldrSmoothed;
    if (!projectConfig.autoBrightness)
    {
        values[7] = "off (web settings)";
        colors[7] = TFT_YELLOW;
    }
    else if (!getLdrState(ldrTrusted, ldrDark, ldrSmoothed))
    {
        values[7] = "schedule only (no LDR)";
    }
    else if (ldrTrusted)
    {
        values[7] = String("LDR: room ") + (ldrDark ? "dark" : "bright");
    }
    else
    {
        values[7] = "schedule (LDR unproven)";
    }

    unsigned long nowMs = millis();
    values[8] = (manualBrightnessUntil > nowMs)
                    ? "manual, " +
                          String((manualBrightnessUntil - nowMs + 59999UL) / 60000UL) +
                          " min left"
                    : "none";

    int ns = projectConfig.nightStartHour;
    int ne = projectConfig.nightEndHour;
    int npct = brightnessPercent(projectConfig.nightBrightness);
    values[9] = (ns == ne)
                    ? "window off (" + String(npct) + "% dark)"
                    : String(ns) + ":00-" + String(ne) + ":00 -> " + String(npct) + "%";
}

void renderStatusValues()
{
    String values[STATUS_MAX_ROWS];
    uint16_t colors[STATUS_MAX_ROWS];
    for (int i = 0; i < STATUS_MAX_ROWS; i++)
    {
        colors[i] = TFT_WHITE;
    }

    switch (statusPageIndex)
    {
    case 1: fillNetworkValues(values, colors); break;
    case 2: fillDataValues(values, colors); break;
    default: fillSystemValues(values, colors); break;
    }

    int rows = statusRowCount(statusPageIndex);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);

    for (int i = 0; i < rows; i++)
    {
        int x = statusValueX();
        int y = statusRowY(i);
        tft.fillRect(x, y, screenWidth - x, statusRowH(i), clockBackgroundColor);
        tft.setTextColor(colors[i], clockBackgroundColor);
        tft.drawString(values[i], x, y);
    }
}

void renderStatusPage()
{
    tft.fillScreen(clockBackgroundColor);

    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString(STATUS_TITLES[statusPageIndex], screenWidth / 2, scaleUiY(2));

    const char *const *labels = statusLabels(statusPageIndex);
    int rows = statusRowCount(statusPageIndex);

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    for (int i = 0; i < rows; i++)
    {
        tft.drawString(labels[i], scaleUiX(8), statusRowY(i));
    }

    tft.setTextFont(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    String footer = (statusPageIndex + 1 < STATUS_PAGE_COUNT)
                        ? "Page " + String(statusPageIndex + 1) + "/" +
                              String(STATUS_PAGE_COUNT) + " - tap for next"
                        : "Page " + String(STATUS_PAGE_COUNT) + "/" +
                              String(STATUS_PAGE_COUNT) + " - tap to go back";
    tft.drawString(footer, screenWidth / 2, scaleUiY(228));

    renderStatusValues();
}

/*-------- Logs page ----------*/
// The tail of the in-RAM log ring (see logBuffer.h), newest lines at the
// bottom, refreshed whenever a new line lands. Tap anywhere to go back.

const int LOGS_TOP = 18;       // first log line y (below the title row)
const int LOGS_LINE_STEP = 10; // font 1 is 8px tall; +2 leading
const int LOGS_MAX_LINES = 32;

uint32_t lastShownLogVersion = 0;

static int logsTop()
{
    return scaleUiY(LOGS_TOP);
}

static int logsVisibleLines()
{
    return constrain((screenHeight - logsTop()) / LOGS_LINE_STEP, 1, LOGS_MAX_LINES);
}

static int logsMaxChars()
{
    return uiTextColsFromX(scaleUiX(4));
}

static void renderLogsLines()
{
    // Grab a bit more than one screenful and keep the last N lines.
    int visibleLines = logsVisibleLines();
    int maxChars = logsMaxChars();
    int tailBudget = visibleLines * (maxChars + 48);
    if (tailBudget > 4096) tailBudget = 4096;
    String text = logTail(tailBudget);
    struct Seg
    {
        uint16_t start;
        uint16_t len;
    };
    Seg segs[LOGS_MAX_LINES];
    int count = 0, next = 0;
    int lineStart = 0;
    int tlen = (int)text.length();
    for (int i = 0; i <= tlen; i++)
    {
        if (i == tlen || text[i] == '\n')
        {
            int len = i - lineStart;
            if (len > 0)
            {
                if (len > maxChars) len = maxChars; // truncate, no wrap
                segs[next] = {(uint16_t)lineStart, (uint16_t)len};
                next = (next + 1) % visibleLines;
                if (count < visibleLines) count++;
            }
            lineStart = i + 1;
        }
    }

    tft.fillRect(0, logsTop(), screenWidth, screenHeight - logsTop(), clockBackgroundColor);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    int first = (next + visibleLines - count) % visibleLines;
    for (int k = 0; k < count; k++)
    {
        const Seg &g = segs[(first + k) % visibleLines];
        tft.drawString(text.substring(g.start, g.start + g.len),
                       scaleUiX(4), logsTop() + k * LOGS_LINE_STEP);
    }
    lastShownLogVersion = logVersion();
}

void renderLogsPage()
{
    tft.fillScreen(clockBackgroundColor);

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("LOGS", scaleUiX(4), 0);

    tft.setTextFont(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString("tap anywhere to go back", screenWidth - scaleUiX(4), scaleUiY(4));

    renderLogsLines();
}

/*-------- Remote UI driving (/api/screen) ----------*/
// Open a page by name, exactly as the touch UI would - see uiPages.h.

bool uiOpenScreenByName(const String &name, int page, int slot)
{
    if (name == "home")
    {
        switchToScreen(SCREEN_HOME);
    }
    else if (name == "settings")
    {
        switchToScreen(SCREEN_SETTINGS);
    }
    else if (name == "zones")
    {
        switchToScreen(SCREEN_ZONE_PICK);
    }
    else if (name == "tzlist")
    {
        zoneSlotBeingEdited = constrain(slot, 0, 3);
        tzListPage = page; // renderTzListPage clamps the range itself
        switchToScreen(SCREEN_TZ_LIST);
    }
    else if (name == "status")
    {
        statusPageIndex = constrain(page, 0, STATUS_PAGE_COUNT - 1);
        switchToScreen(SCREEN_STATUS);
    }
    else if (name == "logs")
    {
        switchToScreen(SCREEN_LOGS);
    }
    else if (name == "wifilogin")
    {
        openWifiLoginHelper(); // brings up the helper AP + NAT, like the button
    }
    else if (name == "wififail")
    {
        // Preview support: seed demo details when no real failure is stored.
        // Preview mode also disables the page's resume/auto-reboot behavior
        // (renderUiPage) so it holds still for a /screenshot.
        if (wifiFailSsid.length() == 0)
        {
            wifiFailSsid = "example-network";
            wifiFailStatus = WL_CONNECT_FAILED;
            wifiFailPreview = true;
        }
        switchToScreen(SCREEN_WIFI_FAIL);
    }
    else if (name == "caltouch")
    {
        openTouchCalibration();
        // No-op on boards whose touch driver needs no calibration
        return uiScreen == SCREEN_TOUCH_CAL;
    }
    else
    {
        return false;
    }
    return true;
}

const char *uiScreenName()
{
    switch (uiScreen)
    {
    case SCREEN_SETTINGS: return "settings";
    case SCREEN_ZONE_PICK: return "zones";
    case SCREEN_TZ_LIST: return "tzlist";
    case SCREEN_STATUS: return "status";
    case SCREEN_LOGS: return "logs";
    case SCREEN_WIFI_LOGIN: return "wifilogin";
    case SCREEN_WIFI_FAIL: return "wififail";
    case SCREEN_TOUCH_CAL: return "caltouch";
    default: return "home";
    }
}

/*-------- Touch routing + page loop ----------*/

void handleUiTouch()
{
    // The calibration screen samples the panel raw itself (renderUiPage);
    // calibrated getTouch() readings are meaningless while it runs.
    if (uiScreen == SCREEN_TOUCH_CAL)
        return;

    int tx = 0, ty = 0;
    if (!uiNewTouch(tx, ty))
        return;

    switch (uiScreen)
    {
    case SCREEN_SETTINGS:
        if (buttonContains(settingsButton(BTN_SET_TZ), tx, ty))
        {
            switchToScreen(SCREEN_ZONE_PICK);
        }
        else if (buttonContains(settingsButton(BTN_SET_FACE), tx, ty))
        {
            projectConfig.clockFace = (projectConfig.clockFace + 1) % FACE_COUNT;
            projectConfig.saveConfigFile();
            uiPageDrawn = false; // redraw with the new label
        }
        else if (buttonContains(settingsButton(BTN_SET_CLK), tx, ty))
        {
            SHOW_24HOUR = !SHOW_24HOUR;
            saveDisplayPrefs();
            uiPageDrawn = false; // redraw with the new label
        }
        else if (buttonContains(settingsButton(BTN_SET_DATE), tx, ty))
        {
            NOT_US_DATE = !NOT_US_DATE;
            saveDisplayPrefs();
            uiPageDrawn = false;
        }
        else if (buttonContains(settingsButton(BTN_SET_GRID), tx, ty))
        {
            projectConfig.showGrid = !projectConfig.showGrid;
            projectConfig.saveConfigFile();
            uiPageDrawn = false; // redraw with the new label
        }
        else if (buttonContains(settingsButton(BTN_SET_WXALERT), tx, ty))
        {
            projectConfig.weatherAlerts = !projectConfig.weatherAlerts;
            projectConfig.saveConfigFile();
            uiPageDrawn = false; // redraw with the new label
        }
        else if (buttonContains(settingsButton(BTN_SET_DIM), tx, ty))
        {
            adjustBacklightFromUi(-15);
        }
        else if (buttonContains(settingsButton(BTN_SET_BRI), tx, ty))
        {
            adjustBacklightFromUi(15);
        }
        else if (buttonContains(settingsButton(BTN_SET_STAT), tx, ty))
        {
            statusPageIndex = 0; // always enter on the first status page
            switchToScreen(SCREEN_STATUS);
        }
        else if (buttonContains(settingsButton(BTN_SET_LOGS), tx, ty))
        {
            switchToScreen(SCREEN_LOGS);
        }
        else if (buttonContains(settingsButton(BTN_SET_TOUCH), tx, ty))
        {
            Log.println("Settings - opening touch calibration");
            openTouchCalibration();
        }
        else if (buttonContains(settingsButton(BTN_SET_WIFI), tx, ty))
        {
            openWifiLoginHelper();
        }
        else if (buttonContains(settingsButton(BTN_SET_BACK), tx, ty))
        {
            switchToScreen(SCREEN_HOME);
        }
        break;

    case SCREEN_WIFI_LOGIN:
        // Any tap on the Done button (or, once online, anywhere) leaves the
        // helper; switchToScreen tears the relay AP + NAT back down.
        if (buttonContains(scaleUiButton(BTN_WIFI_DONE), tx, ty) ||
            wifiRelayState() == RELAY_SUCCESS)
        {
            switchToScreen(SCREEN_SETTINGS);
        }
        break;

    case SCREEN_WIFI_FAIL:
        if (buttonContains(scaleUiButton(BTN_FAIL_REBOOT), tx, ty))
        {
            Log.println("WiFi failure page: Reboot tapped - restarting");
            delay(300); // let the log line reach the serial port
            ESP.restart();
        }
        else if (buttonContains(scaleUiButton(BTN_FAIL_SET), tx, ty))
        {
            switchToScreen(SCREEN_SETTINGS);
        }
        break;

    case SCREEN_ZONE_PICK:
        for (int i = 0; i < 4; i++)
        {
            if (buttonContains(scaleUiButton(BTN_ZONE[i]), tx, ty))
            {
                zoneSlotBeingEdited = i;
                // Open the list on the page containing the current selection
                tzListPage = 0;
                for (int p = 0; p < TZ_PRESET_COUNT; p++)
                {
                    if (worldZones[i].timezone == TZ_PRESETS[p].tz)
                    {
                        tzListPage = p / TZ_PER_PAGE;
                        break;
                    }
                }
                switchToScreen(SCREEN_TZ_LIST);
                return;
            }
        }
        if (buttonContains(scaleUiButton(BTN_ZONE_BACK), tx, ty))
        {
            switchToScreen(SCREEN_SETTINGS);
        }
        break;

    case SCREEN_TZ_LIST:
    {
        int totalPages = (TZ_PRESET_COUNT + TZ_PER_PAGE - 1) / TZ_PER_PAGE;
        for (int row = 0; row < TZ_PER_PAGE; row++)
        {
            int idx = tzListPage * TZ_PER_PAGE + row;
            if (idx >= TZ_PRESET_COUNT)
                break;
            if (buttonContains(tzRowButton(row), tx, ty))
            {
                applyZoneSelection(zoneSlotBeingEdited, TZ_PRESETS[idx]);
                switchToScreen(SCREEN_ZONE_PICK);
                return;
            }
        }
        if (buttonContains(scaleUiButton(BTN_TZ_PREV), tx, ty))
        {
            tzListPage = (tzListPage + totalPages - 1) % totalPages;
            uiPageDrawn = false;
        }
        else if (buttonContains(scaleUiButton(BTN_TZ_NEXT), tx, ty))
        {
            tzListPage = (tzListPage + 1) % totalPages;
            uiPageDrawn = false;
        }
        else if (buttonContains(scaleUiButton(BTN_TZ_BACK), tx, ty))
        {
            switchToScreen(SCREEN_ZONE_PICK);
        }
        break;
    }

    case SCREEN_STATUS:
        // Tap cycles through the status pages; the last one returns to settings
        statusPageIndex++;
        if (statusPageIndex >= STATUS_PAGE_COUNT)
        {
            statusPageIndex = 0;
            switchToScreen(SCREEN_SETTINGS);
        }
        else
        {
            uiPageDrawn = false; // repaint with the next page's rows
            touchSuppressedUntilRelease = true;
        }
        break;

    case SCREEN_LOGS:
        switchToScreen(SCREEN_SETTINGS);
        break;

    default:
        break;
    }
}

void renderUiPage()
{
    if (!uiPageDrawn)
    {
        switch (uiScreen)
        {
        case SCREEN_SETTINGS:
            renderSettingsPage();
            break;
        case SCREEN_ZONE_PICK:
            renderZonePickPage();
            break;
        case SCREEN_TZ_LIST:
            renderTzListPage();
            break;
        case SCREEN_STATUS:
            renderStatusPage();
            lastStatusRefresh = millis();
            break;
        case SCREEN_LOGS:
            renderLogsPage();
            lastStatusRefresh = millis();
            break;
        case SCREEN_WIFI_LOGIN:
            renderWifiLoginPage();
            lastStatusRefresh = millis();
            break;
        case SCREEN_WIFI_FAIL:
            renderWifiFailPage();
            lastStatusRefresh = millis();
            break;
        case SCREEN_TOUCH_CAL:
            renderTouchCalPage();
            break;
        default:
            break;
        }
        uiPageDrawn = true;
    }
    else if (uiScreen == SCREEN_STATUS && millis() - lastStatusRefresh > 1000)
    {
        // Live-refresh the dynamic values (uptime, heap, RSSI, clock) once a second
        renderStatusValues();
        lastStatusRefresh = millis();
    }
    else if (uiScreen == SCREEN_LOGS && millis() - lastStatusRefresh > 1000)
    {
        // Repaint the tail only when a new line has actually arrived
        if (logVersion() != lastShownLogVersion)
        {
            renderLogsLines();
        }
        lastStatusRefresh = millis();
    }
    else if (uiScreen == SCREEN_WIFI_FAIL && millis() - lastStatusRefresh > 1000)
    {
        lastStatusRefresh = millis();
        static unsigned long connectedAtMs = 0;
        if (wifiFailPreview)
        {
            // Demo data opened via /api/screen: the link is usually already
            // up, so the resume path would dismiss the page after seconds
            // (and the recovery path would reboot the device under a
            // developer's feet). The page stays until navigated away.
            connectedAtMs = 0;
        }
        else if (WiFi.status() == WL_CONNECTED)
        {
            // The background retries got us on after all - show the good
            // news for a few seconds, then resume the clock on our own.
            if (connectedAtMs == 0)
            {
                connectedAtMs = millis();
                // The free band between the explanation text (ends at 176)
                // and the buttons (start at 192).
                tft.fillRect(0, scaleUiY(176), screenWidth, scaleUiY(16),
                             clockBackgroundColor);
                tft.setTextFont(2);
                tft.setTextSize(1);
                tft.setTextDatum(TC_DATUM);
                tft.setTextColor(TFT_GREEN, clockBackgroundColor);
                tft.drawString("WiFi connected - resuming...",
                               screenWidth / 2, scaleUiY(176));
                Log.println("WiFi failure page: link came up - resuming");
            }
            else if (millis() - connectedAtMs > 6000)
            {
                connectedAtMs = 0;
                switchToScreen(SCREEN_HOME);
            }
        }
        else
        {
            connectedAtMs = 0;
            if (millis() - wifiFailShownAt > WIFI_FAIL_AUTO_REBOOT_MS)
            {
                Log.println("WiFi failure page: untouched for 5 min - "
                            "rebooting to rerun the boot recovery sequence");
                delay(300);
                ESP.restart();
            }
        }
    }
    else if (uiScreen == SCREEN_WIFI_LOGIN)
    {
        // Drive the login relay every loop (it self-rate-limits its polling);
        // repaint the status line once a second as the state changes.
        static unsigned long successAtMs = 0;
        wifiRelayService();
        if (millis() - lastStatusRefresh > 1000)
        {
            renderWifiLoginStatus();
            lastStatusRefresh = millis();
        }
        // Once online, linger a few seconds so the "Online!" is seen, then
        // return home on its own (unattended recovery needs no tap).
        if (wifiRelayState() == RELAY_SUCCESS)
        {
            if (successAtMs == 0) successAtMs = millis();
            if (millis() - successAtMs > 4000)
            {
                successAtMs = 0;
                switchToScreen(SCREEN_HOME);
            }
        }
        else
        {
            successAtMs = 0;
        }
    }
    else if (uiScreen == SCREEN_TOUCH_CAL)
    {
        // Poll the raw panel every loop: collect corner samples, advance the
        // arrows and finish or time out the calibration.
        serviceTouchCalScreen();
    }
}
