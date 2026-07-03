#ifndef UI_PAGES_H
#define UI_PAGES_H

// ---------------------------------------------------------------------------
// Touch UI pages: settings, system status and timezone selection.
//
// Included from ClockLogic.h (single translation unit) after the display,
// touch and worldZones globals are defined. Navigation:
//   Home screen: tap the CENTER of the screen  -> settings page
//                tap the LEFT / RIGHT third    -> brightness down / up (existing)
//   Settings:    change timezones, clock/date format, brightness, view status
// ---------------------------------------------------------------------------

enum UIScreen
{
    SCREEN_HOME,
    SCREEN_SETTINGS,
    SCREEN_ZONE_PICK,
    SCREEN_TZ_LIST,
    SCREEN_STATUS
};

UIScreen uiScreen = SCREEN_HOME;
bool uiPageDrawn = false;      // false -> render the full page on the next loop
int zoneSlotBeingEdited = 0;   // which quadrant the timezone list is editing
int tzListPage = 0;            // current page of the timezone list
unsigned long lastStatusRefresh = 0;

// After a screen switch, ignore the touch panel until the finger is lifted so
// a single tap can't "click through" onto the newly drawn page.
bool touchSuppressedUntilRelease = false;

/*-------- Timezone presets ----------*/

struct TimezonePreset
{
    const char *name; // label shown on the clock quadrant
    const char *tz;   // tz database name used by ezTime
};

const TimezonePreset TZ_PRESETS[] = {
    {"SANTA CLARA", "America/Los_Angeles"},
    {"DENVER", "America/Denver"},
    {"CHICAGO", "America/Chicago"},
    {"NEW YORK", "America/New_York"},
    {"SAO PAULO", "America/Sao_Paulo"},
    {"LONDON", "Europe/London"},
    {"PARIS", "Europe/Paris"},
    {"BERLIN", "Europe/Berlin"},
    {"MOSCOW", "Europe/Moscow"},
    {"DUBAI", "Asia/Dubai"},
    {"MUMBAI", "Asia/Kolkata"},
    {"SINGAPORE", "Asia/Singapore"},
    {"HONG KONG", "Asia/Hong_Kong"},
    {"BEIJING", "Asia/Shanghai"},
    {"TOKYO", "Asia/Tokyo"},
    {"SEOUL", "Asia/Seoul"},
    {"SYDNEY", "Australia/Sydney"},
    {"AUCKLAND", "Pacific/Auckland"},
};
const int TZ_PRESET_COUNT = sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]);
const int TZ_PER_PAGE = 5;

// Market (trading session) definitions for timezones that host an exchange.
// Timezones without an entry here simply show no market status line.
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

// Copy the timezones saved in the project config onto the four quadrants.
// Called once during setup, before the zones are initialized over NTP.
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

// Settings page layout
const UIButton BTN_SET_TZ = {20, 42, 280, 30};
const UIButton BTN_SET_CLK = {20, 78, 280, 30};
const UIButton BTN_SET_DATE = {20, 114, 280, 30};
const UIButton BTN_SET_DIM = {20, 150, 60, 30};
const UIButton BTN_SET_BRI = {240, 150, 60, 30};
const UIButton BTN_SET_STAT = {20, 196, 135, 32};
const UIButton BTN_SET_BACK = {165, 196, 135, 32};

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
    tft.fillRect(85, 150, 150, 30, clockBackgroundColor);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    int pct = map(backlightLevel, 5, 255, 0, 100);
    tft.drawString("Brightness " + String(pct) + "%", 160, 165);
}

void adjustBacklightFromUi(int delta)
{
    backlightLevel += delta;
    if (backlightLevel < 5) backlightLevel = 5;
    if (backlightLevel > 255) backlightLevel = 255;
    analogWrite(BACKLIGHT_PIN, backlightLevel);
    // Hold this manual setting before auto-brightness resumes
    manualBrightnessUntil = millis() + MANUAL_BRIGHTNESS_HOLD_MS;
    drawSettingsBrightnessLabel();
    Serial.println("Brightness set from settings page: " + String(backlightLevel));
}

// Apply a timezone preset to a quadrant, persist it, and re-fetch the zone
// definition from the ezTime server (brief blocking network call).
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
        Serial.println("Failed to set timezone " + String(preset.tz));
    }
    if (worldZones[slot].market.hasMarket)
    {
        worldZones[slot].lastMarketStatus = getMarketStatus(worldZones[slot]);
    }

    projectConfig.zoneName[slot] = preset.name;
    projectConfig.zoneTZ[slot] = preset.tz;
    projectConfig.saveConfigFile();

    Serial.println("Quadrant " + String(slot) + " set to " + String(preset.name) +
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
    tft.drawString("SETTINGS", 160, 6);

    drawButton(BTN_SET_TZ, "Change timezones  >", TFT_CYAN, TFT_WHITE);
    drawButton(BTN_SET_CLK,
               SHOW_24HOUR ? "Clock format: 24 hour" : "Clock format: 12 hour (AM/PM)",
               TFT_CYAN, TFT_WHITE);
    drawButton(BTN_SET_DATE,
               NOT_US_DATE ? "Date format: DD/MM/YY" : "Date format: MM/DD/YY",
               TFT_CYAN, TFT_WHITE);
    drawButton(BTN_SET_DIM, "-", TFT_CYAN, TFT_WHITE);
    drawButton(BTN_SET_BRI, "+", TFT_CYAN, TFT_WHITE);
    drawSettingsBrightnessLabel();
    drawButton(BTN_SET_STAT, "System status", TFT_GREEN, TFT_WHITE);
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

const int STATUS_ROW_COUNT = 9;
const int STATUS_VALUE_X = 112;
const int STATUS_ROW_Y0 = 38;
const int STATUS_ROW_STEP = 18;

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
    String values[STATUS_ROW_COUNT];
    values[0] = WiFi.SSID();
    values[1] = WiFi.localIP().toString();
    values[2] = String(WiFi.RSSI()) + " dBm";
    values[3] = WiFi.macAddress();
    values[4] = formatUptime();
    values[5] = String(ESP.getFreeHeap() / 1024) + " KB";
    values[6] = String(syncCount);
    values[7] = (syncCount > 0)
                    ? String((millis() - lastSyncTime) / 60000UL) + " min ago"
                    : "not since boot";
    values[8] = UTC.dateTime("H:i:s") + " UTC";

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);

    for (int i = 0; i < STATUS_ROW_COUNT; i++)
    {
        int y = STATUS_ROW_Y0 + i * STATUS_ROW_STEP;
        tft.fillRect(STATUS_VALUE_X, y, 320 - STATUS_VALUE_X, STATUS_ROW_STEP, clockBackgroundColor);

        uint16_t color = TFT_WHITE;
        if (i == 2) // color-code the WiFi signal strength
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
    tft.drawString("SYSTEM STATUS", 160, 4);

    const char *labels[STATUS_ROW_COUNT] = {
        "WiFi SSID", "IP address", "Signal", "MAC", "Uptime",
        "Free heap", "NTP syncs", "Last sync", "UTC time"
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
    tft.drawString("Tap anywhere to go back", 160, 224);

    renderStatusValues();
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
}

#endif // UI_PAGES_H
