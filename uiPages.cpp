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
// Bottom row: four buttons across (Status / Logs / WiFi login helper / Back).
const UIButton BTN_SET_STAT = {20, 208, 62, 26};
const UIButton BTN_SET_LOGS = {88, 208, 46, 26};
const UIButton BTN_SET_WIFI = {140, 208, 92, 26};
const UIButton BTN_SET_BACK = {238, 208, 62, 26};
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
    return b;
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
static const int BOOT_CON_COLS = 52; // 6 px glyphs, 2 px left margin

static String bootConShown[BOOT_CON_ROWS]; // what each row currently displays
static uint32_t bootConLastBytes = 0;
static unsigned long bootConLastDraw = 0;

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
    String tail = logTail(BOOT_CON_ROWS * 96);
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
        if (line.length() > BOOT_CON_COLS)
            line = line.substring(0, BOOT_CON_COLS);
        if (!force && line == bootConShown[i])
            continue;
        bootConShown[i] = line;
        int y = BOOT_CON_TOP + i * BOOT_CON_LINE_H;
        tft.fillRect(0, y, screenWidth, BOOT_CON_LINE_H, clockBackgroundColor);
        tft.drawString(line, 2, y);
    }
}

static void bootUiDrawChrome()
{
    bool tapped = bootSettingsWanted;
    drawButton(BTN_BOOT_SETTINGS, "Settings",
               tapped ? TFT_GREEN : TFT_CYAN, tapped ? TFT_GREEN : TFT_WHITE);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString("WiFi login / status / logs", screenWidth / 2, screenHeight - 10);
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
    tft.drawString("System initializing...", screenWidth / 2, 2);
    // Device identity under the title - see displaySetup.
    tft.setTextFont(1);
    tft.setTextColor(TFT_CYAN, clockBackgroundColor);
    tft.drawString(deviceLabel(), screenWidth / 2, 18);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString(deviceMacAddress(), screenWidth / 2, 28);
    tft.drawFastHLine(0, 38, screenWidth, TFT_DARKGREY);
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
    if (t.zRaw > 800 && buttonContains(BTN_BOOT_SETTINGS, t.x, t.y))
    {
        bootSettingsWanted = true;
        // Acknowledge right away - the remaining boot steps can still take a
        // few seconds before the settings page actually appears.
        drawButton(BTN_BOOT_SETTINGS, "Settings", TFT_GREEN, TFT_GREEN);
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
static unsigned long wifiFailShownAt = 0;

// Auto-reboot the failure page after this long untouched, so an unattended
// clock still runs the boot recovery sequence (portal included) on its own.
static const unsigned long WIFI_FAIL_AUTO_REBOOT_MS = 5UL * 60UL * 1000UL;

void bootReportWifiFailure(const String &ssid, int wlStatus)
{
    wifiFailSsid = ssid;
    wifiFailStatus = wlStatus;
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
    tft.drawString("WIFI CONNECT FAILED", 160, 2);

    const char *problem, *detail;
    wifiFailReason(wifiFailStatus, problem, detail);

    String ssid = wifiFailSsid.length() ? wifiFailSsid : "(no network name)";
    if (ssid.length() > 24) ssid = ssid.substring(0, 24);

    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString("Network:", 12, 36);
    tft.setTextColor(TFT_YELLOW, clockBackgroundColor);
    tft.drawString(ssid, 90, 36);

    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString("Problem:", 12, 56);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString(String(problem) + " (" + String(wifiFailStatus) + ")", 90, 56);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString(detail, 12, 76);

    tft.drawString("Reboot: try again now (the setup", 12, 104);
    tft.drawString("portal reopens if it fails again).", 12, 120);
    tft.drawString("Settings: open status & logs; WiFi", 12, 144);
    tft.drawString("keeps retrying in the background.", 12, 160);

    drawButton(BTN_FAIL_REBOOT, "Reboot", TFT_RED, TFT_WHITE);
    drawButton(BTN_FAIL_SET, "Settings", TFT_CYAN, TFT_WHITE);

    tft.setTextFont(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString("reboots by itself after 5 minutes", 160, 232);

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
    tft.drawString("Loading " + String(preset.name) + "...", 160, 120);

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
    tft.fillRect(0, 166, 320, 20, clockBackgroundColor);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color, clockBackgroundColor);
    tft.drawString(text, 160, 168);
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
    tft.drawString("WI-FI LOGIN", 160, 2);

    tft.setTextFont(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString("1. On your phone, join this Wi-Fi:", 12, 30);

    tft.setTextColor(TFT_YELLOW, clockBackgroundColor);
    tft.drawString(wifiRelayApSsid(), 24, 48);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString(String("password: ") + wifiRelayApPassword(), 24, 64);

    tft.drawString("2. Sign in on the login page that", 12, 84);
    tft.drawString("   opens (none? try neverssl.com).", 12, 100);
    // Phones love to flee a hotspot that has "no internet" - that warning is
    // expected here and leaving the hotspot is the #1 way the login fails.
    tft.drawString("3. Stay on that Wi-Fi - ignore any", 12, 120);
    tft.drawString("   'no internet' warning, wait here.", 12, 136);

    renderWifiLoginStatus();

    drawButton(BTN_WIFI_DONE, "Done", TFT_DARKGREY, TFT_WHITE);
}

void renderZonePickPage()
{
    tft.fillScreen(clockBackgroundColor);

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("TAP A CLOCK TO CHANGE ITS TIMEZONE", 160, 10);

    for (int i = 0; i < 4; i++)
    {
        const UIButton &b = BTN_ZONE[i];
        int cx = b.x + b.w / 2;
        tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, TFT_CYAN);

        tft.setTextFont(1);
        tft.setTextSize(1);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
        tft.drawString(SLOT_LABELS[i], cx, b.y + 8);

        tft.setTextFont(2);
        tft.setTextColor(TFT_YELLOW, clockBackgroundColor);
        tft.drawString(worldZones[i].name, cx, b.y + 26);

        tft.setTextFont(1);
        tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
        tft.drawString(worldZones[i].timezone, cx, b.y + 54);
    }

    drawButton(BTN_ZONE_BACK, "Back", TFT_DARKGREY, TFT_WHITE);
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
    tft.drawString(String(SLOT_LABELS[zoneSlotBeingEdited]) + " CLOCK", 160, 8);

    tft.setTextFont(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString(String(tzListPage + 1) + "/" + String(totalPages), 314, 10);

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

    drawButton(BTN_TZ_PREV, "< Prev", TFT_CYAN, TFT_WHITE);
    drawButton(BTN_TZ_BACK, "Back", TFT_DARKGREY, TFT_WHITE);
    drawButton(BTN_TZ_NEXT, "Next >", TFT_CYAN, TFT_WHITE);
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

static const char *STATUS_TITLES[STATUS_PAGE_COUNT] = {
    "SYSTEM STATUS", "NETWORK & STORAGE", "CLOCK DATA"};

static const char *STATUS_LABELS_SYSTEM[] = {
    "WiFi", "IP addr", "CPU", "CPU temp", "Flash", "Firmware",
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
    values[3] = "no sensor on this chip";
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
        int y = STATUS_ROW_Y0 + i * STATUS_ROW_STEP;
        tft.fillRect(STATUS_VALUE_X, y, 320 - STATUS_VALUE_X, STATUS_ROW_STEP, clockBackgroundColor);
        tft.setTextColor(colors[i], clockBackgroundColor);
        tft.drawString(values[i], STATUS_VALUE_X, y);
    }
}

void renderStatusPage()
{
    tft.fillScreen(clockBackgroundColor);

    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString(STATUS_TITLES[statusPageIndex], 160, 2);

    const char *const *labels = statusLabels(statusPageIndex);
    int rows = statusRowCount(statusPageIndex);

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    for (int i = 0; i < rows; i++)
    {
        tft.drawString(labels[i], 8, STATUS_ROW_Y0 + i * STATUS_ROW_STEP);
    }

    tft.setTextFont(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    String footer = (statusPageIndex + 1 < STATUS_PAGE_COUNT)
                        ? "Page " + String(statusPageIndex + 1) + "/" +
                              String(STATUS_PAGE_COUNT) + " - tap for next"
                        : "Page " + String(STATUS_PAGE_COUNT) + "/" +
                              String(STATUS_PAGE_COUNT) + " - tap to go back";
    tft.drawString(footer, 160, 228);

    renderStatusValues();
}

/*-------- Logs page ----------*/
// The tail of the in-RAM log ring (see logBuffer.h), newest lines at the
// bottom, refreshed whenever a new line lands. Tap anywhere to go back.

const int LOGS_TOP = 18;       // first log line y (below the title row)
const int LOGS_LINE_STEP = 10; // font 1 is 8px tall; +2 leading
const int LOGS_MAX_LINES = 21;
const int LOGS_MAX_CHARS = 52; // 320px / 6px per font-1 char, with margin

uint32_t lastShownLogVersion = 0;

static void renderLogsLines()
{
    // Grab a bit more than one screenful and keep the last N lines.
    String text = logTail(2600);
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
                if (len > LOGS_MAX_CHARS) len = LOGS_MAX_CHARS; // truncate, no wrap
                segs[next] = {(uint16_t)lineStart, (uint16_t)len};
                next = (next + 1) % LOGS_MAX_LINES;
                if (count < LOGS_MAX_LINES) count++;
            }
            lineStart = i + 1;
        }
    }

    tft.fillRect(0, LOGS_TOP, 320, 240 - LOGS_TOP, clockBackgroundColor);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    int first = (next + LOGS_MAX_LINES - count) % LOGS_MAX_LINES;
    for (int k = 0; k < count; k++)
    {
        const Seg &g = segs[(first + k) % LOGS_MAX_LINES];
        tft.drawString(text.substring(g.start, g.start + g.len),
                       4, LOGS_TOP + k * LOGS_LINE_STEP);
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
    tft.drawString("LOGS", 4, 0);

    tft.setTextFont(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString("tap anywhere to go back", 316, 4);

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
        if (wifiFailSsid.length() == 0)
        {
            wifiFailSsid = "example-network";
            wifiFailStatus = WL_CONNECT_FAILED;
        }
        switchToScreen(SCREEN_WIFI_FAIL);
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
    default: return "home";
    }
}

/*-------- Touch routing + page loop ----------*/

void handleUiTouch()
{
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
        if (buttonContains(BTN_WIFI_DONE, tx, ty) ||
            wifiRelayState() == RELAY_SUCCESS)
        {
            switchToScreen(SCREEN_SETTINGS);
        }
        break;

    case SCREEN_WIFI_FAIL:
        if (buttonContains(BTN_FAIL_REBOOT, tx, ty))
        {
            Log.println("WiFi failure page: Reboot tapped - restarting");
            delay(300); // let the log line reach the serial port
            ESP.restart();
        }
        else if (buttonContains(BTN_FAIL_SET, tx, ty))
        {
            switchToScreen(SCREEN_SETTINGS);
        }
        break;

    case SCREEN_ZONE_PICK:
        for (int i = 0; i < 4; i++)
        {
            if (buttonContains(BTN_ZONE[i], tx, ty))
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
        if (buttonContains(BTN_ZONE_BACK, tx, ty))
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
        if (buttonContains(BTN_TZ_PREV, tx, ty))
        {
            tzListPage = (tzListPage + totalPages - 1) % totalPages;
            uiPageDrawn = false;
        }
        else if (buttonContains(BTN_TZ_NEXT, tx, ty))
        {
            tzListPage = (tzListPage + 1) % totalPages;
            uiPageDrawn = false;
        }
        else if (buttonContains(BTN_TZ_BACK, tx, ty))
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
        if (WiFi.status() == WL_CONNECTED)
        {
            // The background retries got us on after all - show the good
            // news for a few seconds, then resume the clock on our own.
            if (connectedAtMs == 0)
            {
                connectedAtMs = millis();
                tft.fillRect(0, 174, 320, 17, clockBackgroundColor);
                tft.setTextFont(2);
                tft.setTextSize(1);
                tft.setTextDatum(TC_DATUM);
                tft.setTextColor(TFT_GREEN, clockBackgroundColor);
                tft.drawString("WiFi connected - resuming...", 160, 175);
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
}
