#include "uiPages.h"

#include <WiFi.h>
#include <soc/soc_caps.h> // SOC_TEMP_SENSOR_SUPPORTED - CPU temp row

#include "clockFaces.h"         // ClockFace enum, clockFaceName
#include "genericBaseProject.h" // BACKLIGHT_PIN, NTP sync state
#include "holidayService.h"     // holidaysInvalidate
#include "projectConfig.h"
#include "weatherService.h"     // weatherInvalidate

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

/*-------- Buttons ----------*/

struct UIButton
{
    int x, y, w, h;
};

bool buttonContains(const UIButton &b, int tx, int ty)
{
    return tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h;
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

// Settings page layout (6 rows, 34px pitch, below the 26px title)
const UIButton BTN_SET_TZ = {20, 30, 280, 28};
const UIButton BTN_SET_FACE = {20, 64, 280, 28};
const UIButton BTN_SET_CLK = {20, 98, 280, 28};
const UIButton BTN_SET_DATE = {20, 132, 280, 28};
const UIButton BTN_SET_DIM = {20, 166, 60, 28};
const UIButton BTN_SET_BRI = {240, 166, 60, 28};
const UIButton BTN_SET_STAT = {20, 200, 85, 28};
const UIButton BTN_SET_LOGS = {113, 200, 89, 28};
const UIButton BTN_SET_BACK = {210, 200, 90, 28};

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

    TouchPoint t = touchscreen.getTouch();
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

/*-------- Settings actions ----------*/

void saveDisplayPrefs()
{
    projectConfig.twentyFourHour = SHOW_24HOUR;
    projectConfig.usDateFormat = !NOT_US_DATE;
    projectConfig.saveConfigFile();
}

void drawSettingsBrightnessLabel()
{
    tft.fillRect(85, 166, 150, 28, clockBackgroundColor);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    int pct = map(backlightLevel, 5, 255, 0, 100);
    tft.drawString("Brightness " + String(pct) + "%", 160, 180);
}

void adjustBacklightFromUi(int delta)
{
    backlightLevel += delta;
    if (backlightLevel < 5) backlightLevel = 5;
    if (backlightLevel > 255) backlightLevel = 255;
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

    if (!worldZones[slot].tz.setLocation(preset.tz))
    {
        // Timezone server unreachable - fall back to the preset's built-in
        // POSIX rules so the quadrant still ticks with correct local time.
        Log.println("Failed to fetch timezone " + String(preset.tz) +
                       " - using built-in POSIX rules");
        worldZones[slot].tz.setPosix(preset.posix);
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
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("SETTINGS", 160, 2);

    drawButton(BTN_SET_TZ, "Change timezones  >", TFT_CYAN, TFT_WHITE);
    drawButton(BTN_SET_FACE,
               "Clock face: " + String(clockFaceName(projectConfig.clockFace)),
               TFT_CYAN, TFT_WHITE);
    drawButton(BTN_SET_CLK,
               SHOW_24HOUR ? "Clock format: 24 hour" : "Clock format: 12 hour (AM/PM)",
               TFT_CYAN, TFT_WHITE);
    drawButton(BTN_SET_DATE,
               NOT_US_DATE ? "Date format: DD/MM/YY" : "Date format: MM/DD/YY",
               TFT_CYAN, TFT_WHITE);
    drawButton(BTN_SET_DIM, "-", TFT_CYAN, TFT_WHITE);
    drawButton(BTN_SET_BRI, "+", TFT_CYAN, TFT_WHITE);
    drawSettingsBrightnessLabel();
    drawButton(BTN_SET_STAT, "Status", TFT_GREEN, TFT_WHITE);
    drawButton(BTN_SET_LOGS, "Logs", TFT_GREEN, TFT_WHITE);
    drawButton(BTN_SET_BACK, "Back", TFT_DARKGREY, TFT_WHITE);
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

/*-------- System status page ----------*/

const int STATUS_ROW_COUNT = 11;
const int STATUS_VALUE_X = 96;
const int STATUS_ROW_Y0 = 34;
const int STATUS_ROW_STEP = 17;

String formatUptime()
{
    unsigned long s = millis() / 1000;
    char buf[24];
    sprintf(buf, "%lud %02lu:%02lu:%02lu",
            s / 86400UL, (s / 3600UL) % 24UL, (s / 60UL) % 60UL, s % 60UL);
    return String(buf);
}

void renderStatusValues()
{
    String ssid = WiFi.SSID();
    if (ssid.length() > 14) ssid = ssid.substring(0, 14);

    uint32_t sketch = ESP.getSketchSize();
    uint32_t slot = sketch + ESP.getFreeSketchSpace();

    String values[STATUS_ROW_COUNT];
    values[0] = ssid + " (" + String(WiFi.RSSI()) + " dBm)";
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
                    : "none since boot";
    values[10] = UTC.dateTime("H:i:s") + " UTC";

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);

    for (int i = 0; i < STATUS_ROW_COUNT; i++)
    {
        int y = STATUS_ROW_Y0 + i * STATUS_ROW_STEP;
        tft.fillRect(STATUS_VALUE_X, y, 320 - STATUS_VALUE_X, STATUS_ROW_STEP, clockBackgroundColor);

        uint16_t color = TFT_WHITE;
        if (i == 0) // color-code the WiFi signal strength
        {
            int rssi = WiFi.RSSI();
            color = (rssi > -60) ? TFT_GREEN : (rssi > -75) ? TFT_YELLOW : TFT_RED;
        }
        tft.setTextColor(color, clockBackgroundColor);
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
    tft.drawString("SYSTEM STATUS", 160, 2);

    const char *labels[STATUS_ROW_COUNT] = {
        "WiFi", "IP addr", "CPU", "CPU temp", "Flash", "Firmware",
        "Build", "Heap", "Uptime", "NTP sync", "UTC time"
    };

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    for (int i = 0; i < STATUS_ROW_COUNT; i++)
    {
        tft.drawString(labels[i], 8, STATUS_ROW_Y0 + i * STATUS_ROW_STEP);
    }

    tft.setTextFont(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString("Tap anywhere to go back", 160, 228);

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

/*-------- Touch routing + page loop ----------*/

void handleUiTouch()
{
    int tx = 0, ty = 0;
    if (!uiNewTouch(tx, ty))
        return;

    switch (uiScreen)
    {
    case SCREEN_SETTINGS:
        if (buttonContains(BTN_SET_TZ, tx, ty))
        {
            switchToScreen(SCREEN_ZONE_PICK);
        }
        else if (buttonContains(BTN_SET_FACE, tx, ty))
        {
            projectConfig.clockFace = (projectConfig.clockFace + 1) % FACE_COUNT;
            projectConfig.saveConfigFile();
            uiPageDrawn = false; // redraw with the new label
        }
        else if (buttonContains(BTN_SET_CLK, tx, ty))
        {
            SHOW_24HOUR = !SHOW_24HOUR;
            saveDisplayPrefs();
            uiPageDrawn = false; // redraw with the new label
        }
        else if (buttonContains(BTN_SET_DATE, tx, ty))
        {
            NOT_US_DATE = !NOT_US_DATE;
            saveDisplayPrefs();
            uiPageDrawn = false;
        }
        else if (buttonContains(BTN_SET_DIM, tx, ty))
        {
            adjustBacklightFromUi(-15);
        }
        else if (buttonContains(BTN_SET_BRI, tx, ty))
        {
            adjustBacklightFromUi(15);
        }
        else if (buttonContains(BTN_SET_STAT, tx, ty))
        {
            switchToScreen(SCREEN_STATUS);
        }
        else if (buttonContains(BTN_SET_LOGS, tx, ty))
        {
            switchToScreen(SCREEN_LOGS);
        }
        else if (buttonContains(BTN_SET_BACK, tx, ty))
        {
            switchToScreen(SCREEN_HOME);
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
}
