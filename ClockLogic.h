/*-------- CYD (Cheap Yellow Display) ----------*/

// TFT_eSPI tft = TFT_eSPI();              // Invoke custom library
//  Will be defined outside of here
TFT_eSprite sprite = TFT_eSprite(&tft); // Sprite class

// Touch screen setup
#include <XPT2046_Bitbang.h>

#define MOSI_PIN 32
#define MISO_PIN 39
#define CLK_PIN  25
#define CS_PIN   33

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

XPT2046_Bitbang touchscreen(MOSI_PIN, MISO_PIN, CLK_PIN, CS_PIN);

int clockFont = 4;  // Changed to font 4 for better readability 
int clockSize = 2;  // Moderate size for Font 4 to fit in quadrants
int clockDatum = TL_DATUM;
uint16_t clockBackgroundColor = TFT_BLACK;
uint16_t clockFontColor = TFT_WHITE;  // Changed to white for better contrast
int prevDay = 0;

bool SHOW_24HOUR = true;

bool NOT_US_DATE = true;

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
    int marketId; // MARKET_* id (used for holiday lookup)
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

// The four on-screen zones. Populated from projectConfig at startup by
// initWorldZonesFromConfig().
WorldClockZone worldZones[4];

// Index of the "home" zone (drives auto-brightness and the +1/-1 relative-date
// hint). Set from projectConfig at startup.
int homeZoneIndex = 0;

// ---- Market catalog -------------------------------------------------------
// Trading sessions are defined in each exchange's LOCAL time. The session
// engine treats Saturday/Sunday and listed holidays as closed.
MarketInfo makeMarket(int id)
{
    MarketInfo m;
    m.marketId = id;
    switch (id)
    {
    case MARKET_NYSE:
        m.exchange = "NYSE"; m.hasMarket = true; m.sessionCount = 3;
        m.sessions[0] = {4, 0, 9, 30, "PRE-MARKET"};
        m.sessions[1] = {9, 30, 16, 0, "REGULAR"};
        m.sessions[2] = {16, 0, 20, 0, "AFTER-HRS"};
        break;
    case MARKET_SSE:
        m.exchange = "SSE"; m.hasMarket = true; m.sessionCount = 4;
        m.sessions[0] = {9, 0, 9, 30, "PRE-MARKET"};
        m.sessions[1] = {9, 30, 11, 30, "REGULAR"};   // Morning session
        m.sessions[2] = {13, 0, 15, 0, "REGULAR"};    // Afternoon session
        m.sessions[3] = {15, 0, 15, 30, "AFTER-HRS"};
        break;
    case MARKET_LSE:
        m.exchange = "LSE"; m.hasMarket = true; m.sessionCount = 4;
        m.sessions[0] = {7, 15, 8, 0, "PRE-MARKET"};
        m.sessions[1] = {8, 0, 16, 30, "REGULAR"};
        m.sessions[2] = {16, 30, 17, 0, "CLOSING"};
        m.sessions[3] = {17, 0, 17, 30, "AFTER-HRS"};
        break;
    default: // MARKET_NONE
        m.exchange = ""; m.hasMarket = false; m.sessionCount = 0;
        break;
    }
    return m;
}

// ---- Market holidays ------------------------------------------------------
// Exchanges close on public holidays in addition to weekends. These tables list
// full-day closures for 2025-2026 and SHOULD BE UPDATED ANNUALLY. The Chinese
// (SSE) calendar is lunar/government-set and only approximate here.
struct Holiday { int y, m, d; };

const Holiday NYSE_HOLIDAYS[] = {
    {2025,1,1},{2025,1,20},{2025,2,17},{2025,4,18},{2025,5,26},{2025,6,19},
    {2025,7,4},{2025,9,1},{2025,11,27},{2025,12,25},
    {2026,1,1},{2026,1,19},{2026,2,16},{2026,4,3},{2026,5,25},{2026,6,19},
    {2026,7,3},{2026,9,7},{2026,11,26},{2026,12,25},
};
const Holiday LSE_HOLIDAYS[] = {
    {2025,1,1},{2025,4,18},{2025,4,21},{2025,5,5},{2025,5,26},{2025,8,25},
    {2025,12,25},{2025,12,26},
    {2026,1,1},{2026,4,3},{2026,4,6},{2026,5,4},{2026,5,25},{2026,8,31},
    {2026,12,25},{2026,12,28},
};
const Holiday SSE_HOLIDAYS[] = { // Approximate - lunar dates, verify each year
    {2025,1,1},{2025,1,28},{2025,1,29},{2025,1,30},{2025,1,31},{2025,2,3},{2025,2,4},
    {2025,4,4},{2025,5,1},{2025,5,2},{2025,5,5},{2025,6,2},
    {2025,10,1},{2025,10,2},{2025,10,3},{2025,10,6},{2025,10,7},{2025,10,8},
    {2026,1,1},{2026,2,16},{2026,2,17},{2026,2,18},{2026,2,19},{2026,2,20},
    {2026,5,1},{2026,10,1},{2026,10,2},
};

bool isMarketHoliday(int marketId, int y, int mth, int d)
{
    const Holiday *table = nullptr;
    int count = 0;
    if (marketId == MARKET_NYSE) { table = NYSE_HOLIDAYS; count = sizeof(NYSE_HOLIDAYS) / sizeof(Holiday); }
    else if (marketId == MARKET_LSE) { table = LSE_HOLIDAYS; count = sizeof(LSE_HOLIDAYS) / sizeof(Holiday); }
    else if (marketId == MARKET_SSE) { table = SSE_HOLIDAYS; count = sizeof(SSE_HOLIDAYS) / sizeof(Holiday); }
    for (int i = 0; i < count; i++)
    {
        if (table[i].y == y && table[i].m == mth && table[i].d == d) return true;
    }
    return false;
}

// Build the worldZones array from the saved/edited configuration.
void initWorldZonesFromConfig()
{
    homeZoneIndex = projectConfig.homeZoneIndex;
    if (homeZoneIndex < 0 || homeZoneIndex >= PROJECT_NUM_ZONES) homeZoneIndex = 0;

    for (int i = 0; i < PROJECT_NUM_ZONES; i++)
    {
        worldZones[i].name = projectConfig.zoneLabels[i];
        worldZones[i].timezone = projectConfig.zoneTimezones[i];
        worldZones[i].lastHour = -1;
        worldZones[i].lastMinute = -1;
        worldZones[i].lastIspm = false;
        worldZones[i].lastDay = -1;
        worldZones[i].initialized = false;
        worldZones[i].market = makeMarket(projectConfig.zoneMarketId[i]);
        worldZones[i].lastMarketStatus = "";
    }
}

// Include serial commands and web config after WorldClockZone is defined
#include "serialCommands.h"

// Global variables for touch and backlight control
bool firstDraw = true;
int backlightLevel = 80; // PWM value (0-255)

// Manual brightness override: when the user changes brightness (touch or serial),
// auto-brightness is suspended until this timestamp so the two don't fight.
unsigned long manualBrightnessUntil = 0;
const unsigned long MANUAL_BRIGHTNESS_HOLD_MS = 2UL * 60UL * 60UL * 1000UL; // 2 hours

// How long the on-screen brightness bar stays visible after the last touch.
const unsigned long BRIGHTNESS_BAR_TIMEOUT_MS = 2000; // 2 seconds

// Global variables for flashing market status messages
unsigned long lastFlashTime = 0;
bool flashState = true;
const unsigned long flashInterval = 1000; // 1 second
bool flashJustChanged = false;

// Function declarations for flashing functionality
bool shouldMessageFlash(String message);
void updateFlashState();
void resetFlashChangeFlag();
void updateMarketStatusOnly(WorldClockZone &zone, int quadrantIndex);
bool needsFlashOnlyUpdate(WorldClockZone &zone);
void adjustBrightnessBasedOnHomeTime();

void SetupCYD()
{
    Serial.println("SetupCYD");
    // tft.init();
    tft.fillScreen(clockBackgroundColor);
    tft.setTextColor(clockFontColor, clockBackgroundColor);

    tft.setRotation(1);
    tft.setTextFont(clockFont);
    tft.setTextSize(clockSize);
    tft.setTextDatum(clockDatum);

    sprite.createSprite(tft.textWidth("8"), tft.fontHeight());
    sprite.setTextColor(clockFontColor, clockBackgroundColor);
    sprite.setRotation(1);
    sprite.setTextFont(clockFont);
    sprite.setTextSize(clockSize);
    sprite.setTextDatum(clockDatum);
}

/*-------- Digits ----------*/
#include "Digit.h"
Digit *digs[4]; // Only need 4 digits for HH:MM (no seconds)
int colons[1]; // Only one colon between hours and minutes

// Screen dimensions (320x240)
int screenWidth = 320;
int screenHeight = 240;
int quadrantWidth = 160;  // 320/2
int quadrantHeight = 120; // 240/2

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

bool ispm; // set by ParseDigits; drives the AM/PM indicator in 12-hour mode

void CalculateDigitOffsetsForQuadrant(int quadrantIndex)
{
    QuadrantPos quad = quadrants[quadrantIndex];
    
    tft.setTextFont(clockFont);
    tft.setTextSize(clockSize);
    int DigitWidth = tft.textWidth("8");
    int colonWidth = tft.textWidth(":");
    int colonPadding = 1;
    int fontHeight = tft.fontHeight();
    
    // Calculate total width of time display (HH:MM only - no AM/PM)
    int timeWidth = DigitWidth * 4 + colonWidth + (colonPadding * 2);
    
    // Center the time display horizontally in the quadrant
    int startX = quad.x + (quadrantWidth - timeWidth) / 2;
    
    // Position time higher up in the quadrant (account for timezone label and date below)
    int labelHeight = 20; // Space for timezone label at top
    int dateHeight = 40;  // Further increased space for date and day name at bottom
    int availableHeight = quadrantHeight - labelHeight - dateHeight;
    int timeY = quad.y + labelHeight + (availableHeight - fontHeight) / 3; // Moved up by using /3 instead of /2
    
    digs[0]->SetXY(startX, timeY);                      // HH
    digs[1]->SetXY(digs[0]->X() + DigitWidth, timeY);   // HH

    colons[0] = digs[1]->X() + DigitWidth + colonPadding; // :

    digs[2]->SetXY(colons[0] + colonWidth + colonPadding, timeY);      // MM
    digs[3]->SetXY(digs[2]->X() + DigitWidth, timeY);   // MM
}

void SetupDigits()
{
    tft.fillScreen(clockBackgroundColor);
    tft.setTextFont(clockFont);
    tft.setTextSize(clockSize);
    tft.setTextDatum(clockDatum);

    for (size_t i = 0; i < 4; i++)  // Only 4 digits for HH:MM
    {
        digs[i] = new Digit(0);
        digs[i]->Height(tft.fontHeight());
    }
    
    // Note: Digit positions will be calculated per quadrant in DrawSingleTimeZone
}

/*-------- DRAWING ----------*/
// Note: DrawColons is now handled within DrawSingleTimeZone for each quadrant

// Note: DrawAmPm is now handled within DrawSingleTimeZone for each quadrant

void DrawADigit(Digit *digg); // Without this line, compiler says: error: variable or field 'DrawADigit' declared void.

void DrawADigit(Digit *digg)
{
    if (digg->Value() == digg->NewValue())
    {
        sprite.drawNumber(digg->Value(), 0, 0);
        sprite.pushSprite(digg->X(), digg->Y());
    }
    else
    {
        for (size_t f = 0; f <= digg->Height(); f++)
        {
            digg->Frame(f);
            sprite.drawNumber(digg->Value(), 0, -digg->Frame());
            sprite.drawNumber(digg->NewValue(), 0, digg->Height() - digg->Frame());
            sprite.pushSprite(digg->X(), digg->Y());
            delay(5);
        }
        digg->Value(digg->NewValue());
    }
}

void DrawDigitsAtOnce()
{
    tft.setTextDatum(TL_DATUM);
    for (size_t f = 0; f <= digs[0]->Height(); f++) // For all animation frames...
    {
        for (size_t di = 0; di < 4; di++) // for all Digits (HH:MM only)...
        {
            Digit *dig = digs[di];
            if (dig->Value() == dig->NewValue()) // If Digit is not changing...
            {
                if (f == 0) //... and this is first frame, just draw it to screeen without animation.
                {
                    sprite.drawNumber(dig->Value(), 0, 0);
                    sprite.pushSprite(dig->X(), dig->Y());
                }
            }
            else // However, if a Digit is changing value, we need to draw animation frame "f"
            {
                dig->Frame(f);                                                       // Set the animation offset
                sprite.drawNumber(dig->Value(), 0, -dig->Frame());                   // Scroll up the current value
                sprite.drawNumber(dig->NewValue(), 0, dig->Height() - dig->Frame()); // while make new value appear from below
                sprite.pushSprite(dig->X(), dig->Y());                               // Draw the current animation frame to actual screen.
            }
        }
        delay(5);
    }

    // Once all animations are done, then we can update all Digits to current new values.
    for (size_t di = 0; di < 4; di++)
    {
        Digit *dig = digs[di];
        dig->Value(dig->NewValue());
    }
}

void DrawDigitsWithoutAnimation()
{
    for (size_t di = 0; di < 4; di++)  // Only 4 digits for HH:MM
    {
        Digit *dig = digs[di];
        dig->Value(dig->NewValue());
        dig->Frame(0);
        sprite.drawNumber(dig->NewValue(), 0, 0);
        sprite.pushSprite(dig->X(), dig->Y());
    }
}

void DrawDigitsOneByOne()
{
    tft.setTextDatum(TL_DATUM);
    for (size_t i = 0; i < 4; i++)  // Only 4 digits for HH:MM
    {
        DrawADigit(digs[3 - i]);
    }
}

void ParseDigits(Timezone &tz)
{
    time_t local = tz.now();
    int hr = hour(local); // 24-hour value from ezTime

    // Track AM/PM (used by the indicator in 12-hour mode)
    ispm = (hr >= 12);

    // Honor the 12/24-hour user setting
    if (!SHOW_24HOUR)
    {
        hr = hr % 12;
        if (hr == 0) hr = 12; // midnight / noon shown as 12, not 0
    }

    digs[0]->NewValue(hr / 10);
    digs[1]->NewValue(hr % 10);
    digs[2]->NewValue(minute(local) / 10);
    digs[3]->NewValue(minute(local) % 10);
}
uint16_t getDayNightColor(Timezone &tz)
{
    time_t local = tz.now();
    int hr = hour(local);

    if (hr >= 6 && hr < 18) {
        return TFT_ORANGE;      // Daytime (6 AM - 6 PM)
    } else if (hr >= 18 && hr < 24) {
        return TFT_LIGHTGREY;        // Evening (6 PM - 12 AM)
    } else {
        return TFT_DARKGREY;   // Night (12 AM - 6 AM)
    }
}

uint16_t getDayNightLabelColor(Timezone &tz)
{
    time_t local = tz.now();
    int hr = hour(local);

    if (hr >= 6 && hr < 18) {
        return TFT_YELLOW;      // Daytime
    } else if (hr >= 18 && hr < 24) {
        return TFT_LIGHTGREY;        // Evening
    } else {
        return TFT_DARKGREY;    // Night
    }
}


// Days since 1970-01-01 for a given civil date (Howard Hinnant's algorithm).
// Used to compare calendar dates between timezones robustly across month/year
// boundaries instead of comparing bare day-of-month numbers.
long daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// Determine the trading status of a zone's exchange in its local time.
// Returns strings like "NYSE OPEN", "NYSE PRE-MARKET OPEN",
// "NYSE CLOSE IN 5 MIN", "NYSE OPEN IN 3 MIN", "NYSE CLOSED", "NYSE HOLIDAY".
// Weekends and listed holidays are treated as closed; sessions that wrap past
// midnight are handled within the same weekday.
String getMarketStatus(WorldClockZone &zone)
{
    if (!zone.market.hasMarket) {
        return ""; // No market for this zone
    }

    time_t local = zone.tz.now();
    int dow = weekday(local); // 1=Sunday ... 7=Saturday
    int nowMin = hour(local) * 60 + minute(local);
    const String &ex = zone.market.exchange;

    // Weekend
    if (dow == 1 || dow == 7) {
        return ex + " CLOSED";
    }

    // Holiday
    if (isMarketHoliday(zone.market.marketId, year(local), month(local), day(local))) {
        return ex + " HOLIDAY";
    }

    // Currently inside a session?
    for (int i = 0; i < zone.market.sessionCount; i++) {
        TradingSession &s = zone.market.sessions[i];
        int start = s.openHour * 60 + s.openMinute;
        int end = s.closeHour * 60 + s.closeMinute;
        bool inSession = (end < start)
            ? (nowMin >= start || nowMin < end)   // wraps past midnight
            : (nowMin >= start && nowMin < end);

        if (inSession) {
            int minutesToClose = (end < start && nowMin >= start)
                ? (24 * 60 - nowMin + end)
                : (end - nowMin);
            String prefix = (s.sessionName == "REGULAR") ? "" : s.sessionName + " ";
            if (minutesToClose <= MARKET_STATUS_MESSAGE_MIN) {
                return ex + " " + prefix + "CLOSE IN " + String(minutesToClose) + " MIN";
            }
            return ex + " " + prefix + "OPEN";
        }
    }

    // Not in a session - is the next one opening soon (later today)?
    int bestMinutesToOpen = -1;
    String bestName = "";
    for (int i = 0; i < zone.market.sessionCount; i++) {
        TradingSession &s = zone.market.sessions[i];
        int start = s.openHour * 60 + s.openMinute;
        if (start > nowMin) {
            int delta = start - nowMin;
            if (bestMinutesToOpen < 0 || delta < bestMinutesToOpen) {
                bestMinutesToOpen = delta;
                bestName = s.sessionName;
            }
        }
    }
    if (bestMinutesToOpen >= 0 && bestMinutesToOpen <= MARKET_STATUS_MESSAGE_MIN) {
        String prefix = (bestName == "REGULAR") ? "" : bestName + " ";
        return ex + " " + prefix + "OPEN IN " + String(bestMinutesToOpen) + " MIN";
    }

    return ex + " CLOSED";
}

uint16_t getMarketStatusColor(String status)
{
    // Check for specific session types first (before generic OPEN check)
    if (status.indexOf("AFTER-HRS OPEN") != -1) {
        return TFT_CYAN;    // Cyan for extended/after-hours trading
    } else if (status.indexOf("PRE-MARKET OPEN") != -1) {
        return TFT_BLUE;    // Blue for pre-market
    } else if (status.indexOf("CLOSING OPEN") != -1) {
        return TFT_YELLOW;  // Yellow for closing auction period
    } else if (status.indexOf("CLOSED") != -1 || status.indexOf("HOLIDAY") != -1) {
        return TFT_RED;     // Red for closed market / holiday
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

void DrawTimeZoneLabel(String label, int quadrantIndex)
{
    QuadrantPos quad = quadrants[quadrantIndex];
    
    tft.setTextFont(1);  // Back to font 1 for timezone labels
    tft.setTextSize(2);  // Back to size 2 for timezone text
    tft.setTextDatum(TC_DATUM); // Top center
    
    // Use day/night color for timezone labels
    tft.setTextColor(getDayNightLabelColor(worldZones[quadrantIndex].tz), clockBackgroundColor);
    
    // Position label at top-center of quadrant
    tft.drawString(label, quad.centerX, quad.y + 5);
}

void DrawDateAndDay(WorldClockZone &zone, int quadrantIndex)
{
    QuadrantPos quad = quadrants[quadrantIndex];
    time_t local = zone.tz.now();
    
    // Get date components
    int dd = day(local);
    int mth = month(local);
    int yr = year(local);
    int dow = weekday(local);
    
    // Day names array
    String dayNames[] = {"", "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    
    // Format date string
    char dateBuffer[12];
    if (NOT_US_DATE) {
        sprintf(dateBuffer, "%02d/%02d/%02d", dd, mth, yr % 100); // DD/MM/YY
    } else {
        sprintf(dateBuffer, "%02d/%02d/%02d", mth, dd, yr % 100); // MM/DD/YY
    }
    
    // Use day/night color for date/day text
    uint16_t dateColor = getDayNightLabelColor(zone.tz);
    
    // For all timezones, add relative date indication compared to home (top-left).
    // Compare actual calendar dates (not bare day-of-month) so it stays correct
    // across month and year boundaries.
    String dayText = dayNames[dow];
    time_t homeTime = worldZones[homeZoneIndex].tz.now();
    long dayDiff = daysFromCivil(yr, mth, dd) -
                   daysFromCivil(year(homeTime), month(homeTime), day(homeTime));

    if (dayDiff >= 1) {
        dayText += " (+1)"; // Ahead of home (tomorrow)
    } else if (dayDiff <= -1) {
        dayText += " (-1)"; // Behind home (yesterday)
    }
    // If same day as home, no indicator needed
    
    // Draw day name - larger text and better positioning
    tft.setTextFont(1);  // Back to font 1 for day names
    tft.setTextSize(2);  // Back to size 2 for day name
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(dateColor, clockBackgroundColor);
    tft.drawString(dayText, quad.centerX, quad.y + quadrantHeight - 50);
    
    // Draw date - larger text and better spacing
    tft.setTextFont(1);  // Keep font 1 for date
    tft.setTextSize(2);  // Back to size 2 for date
    tft.drawString(dateBuffer, quad.centerX, quad.y + quadrantHeight - 30);
    
    // Draw market status if this zone has a market.
    // Use the cached status (refreshed once per minute in hasTimeChanged) rather
    // than recomputing the String here on every redraw.
    String marketStatus = zone.lastMarketStatus;
    if (marketStatus.length() > 0) {
        uint16_t marketColor = getMarketStatusColor(marketStatus);
        tft.setTextFont(1);
        tft.setTextSize(1);  // Smaller text for market status
        
        // Check if this message should flash and apply flashing effect
        if (shouldMessageFlash(marketStatus)) {
            if (flashState) {
                // Flash-on state: display the message normally
                tft.setTextColor(marketColor, clockBackgroundColor);
                tft.drawString(marketStatus, quad.centerX, quad.y + quadrantHeight - 10);
            } else {
                // Flash-off state: clear the text area by drawing a filled rectangle
                int textWidth = tft.textWidth(marketStatus);
                int textHeight = tft.fontHeight();
                int textX = quad.centerX - textWidth / 2;  // Center the text
                int textY = quad.y + quadrantHeight - 10;
                tft.fillRect(textX, textY, textWidth, textHeight, clockBackgroundColor);
            }
        } else {
            // Non-flashing messages display normally
            tft.setTextColor(marketColor, clockBackgroundColor);
            tft.drawString(marketStatus, quad.centerX, quad.y + quadrantHeight - 10);
        }
    }
}

void DrawQuadrantBorders()
{
    // Grid lines between quadrants are intentionally disabled for a cleaner look.
    // Kept as a hook in case borders are wanted again.
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

void resetFlashChangeFlag()
{
    flashJustChanged = false;
}

void updateMarketStatusOnly(WorldClockZone &zone, int quadrantIndex)
{
    QuadrantPos quad = quadrants[quadrantIndex];
    String marketStatus = zone.lastMarketStatus; // cached, refreshed per minute

    if (marketStatus.length() > 0 && shouldMessageFlash(marketStatus)) {
        // Calculate the exact area where market status is displayed
        tft.setTextFont(1);
        tft.setTextSize(1);
        int textWidth = tft.textWidth(marketStatus);
        int textHeight = tft.fontHeight();
        int textX = quad.centerX - textWidth / 2;
        int textY = quad.y + quadrantHeight - 10;
        
        // Clear only the market status area
        tft.fillRect(textX - 2, textY - 1, textWidth + 8, textHeight + 2, clockBackgroundColor);
        
        // Redraw the market status with current flash state
        if (flashState) {
            uint16_t marketColor = getMarketStatusColor(marketStatus);
            tft.setTextColor(marketColor, clockBackgroundColor);
            tft.drawString(marketStatus, quad.centerX, textY);
        }
        // If flashState is false, we just leave the cleared area empty (invisible)
    }
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
            Serial.println("Invalid time detected for " + zone.name + ", reinitializing timezone...");
            zone.tz.setLocation(zone.timezone);
            local = zone.tz.now();
        }
        
        // If still invalid, force update anyway to show something
        if (local < 1000000000) {
            Serial.println("Still invalid time for " + zone.name + ", forcing update");
            zone.initialized = false; // Force redraw
            return true;
        }
    }
    
    int currentHour = hour(local); // Always use 24-hour format
    int currentMinute = minute(local);
    int currentDay = day(local);

    bool isHomeZone = (&zone == &worldZones[homeZoneIndex]);

    // Debug output every 10 seconds for the home zone only to avoid spam
    if (DEBUG_CLOCK && isHomeZone && currentMillis - lastDebugOutput >= 10000) {
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
        if (DEBUG_CLOCK && isHomeZone && zone.initialized) {
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

bool needsFlashOnlyUpdate(WorldClockZone &zone)
{
    if (flashJustChanged && zone.market.hasMarket) {
        return shouldMessageFlash(zone.lastMarketStatus); // cached status
    }
    return false;
}

void adjustBrightnessBasedOnHomeTime()
{
    static int lastHourChecked = -1;
    static unsigned long lastBrightnessAdjustment = 0;
    
    // Don't fight a manual brightness change (touch / serial) - let it hold first
    unsigned long currentTime = millis();
    if (currentTime < manualBrightnessUntil) {
        return;
    }

    // Only check brightness adjustment every 30 seconds to avoid excessive updates
    if (currentTime - lastBrightnessAdjustment < 30000) {
        return;
    }

    // Get current hour from the configured home zone
    if (worldZones[homeZoneIndex].initialized) {
        time_t homeTime = worldZones[homeZoneIndex].tz.now();
        int currentHour = hour(homeTime);
        
        // Only adjust if the hour has changed to avoid constant adjustments
        if (currentHour != lastHourChecked) {
            lastHourChecked = currentHour;
            lastBrightnessAdjustment = currentTime;
            
            int targetBrightness;
            if (currentHour >= 0 && currentHour < 7) {
                // Night time (0-6 AM): dim brightness
                targetBrightness = 5;
            } else {
                // Day time (7 AM - 11:59 PM): normal brightness
                targetBrightness = 80;
            }
            
            // Only adjust if brightness level is different
            if (backlightLevel != targetBrightness) {
                backlightLevel = targetBrightness;
                analogWrite(BACKLIGHT_PIN, backlightLevel);

                Serial.print("Auto brightness adjusted for home zone time ");
                Serial.print(currentHour);
                Serial.print(":xx - Brightness set to ");
                Serial.println(backlightLevel);
            }
        }
    }
}


void DrawQuadrantBackground(int quadrantIndex)
{
    QuadrantPos quad = quadrants[quadrantIndex];
    // Clear only this quadrant (grid lines are disabled - see DrawQuadrantBorders)
    tft.fillRect(quad.x, quad.y, quadrantWidth, quadrantHeight, clockBackgroundColor);
}

void DrawSingleTimeZone(WorldClockZone &zone, int quadrantIndex, bool forceRedraw = false)
{
    // When called from drawRollingClock with forceRedraw=false,
    // we already know the time has changed, so don't check again
    if (!forceRedraw) {
        CLOCK_DEBUG_PRINTLN("Drawing " + zone.name + " - time already confirmed to have changed");
    } else {
        CLOCK_DEBUG_PRINTLN("Drawing " + zone.name + " - force redraw requested");
    }

    // Clear only this quadrant to avoid flashing
    DrawQuadrantBackground(quadrantIndex);

    // Calculate digit positions for this quadrant
    CLOCK_DEBUG_PRINTLN("Calculating digit offsets for quadrant " + String(quadrantIndex));
    CalculateDigitOffsetsForQuadrant(quadrantIndex);
    
    // Parse time for this timezone
    ParseDigits(zone.tz);
    
    // Get day/night appropriate colors
    uint16_t timeColor = getDayNightColor(zone.tz);
    
    // Draw timezone label at top of quadrant
    DrawTimeZoneLabel(zone.name, quadrantIndex);
    
    // Draw time digits without animation for smooth display
    CLOCK_DEBUG_PRINTLN("Drawing digits for " + zone.name + " at position (" + String(quadrantIndex) + ")");
    for (size_t di = 0; di < 4; di++)
    {
        Digit *dig = digs[di];
        dig->Value(dig->NewValue());
        dig->Frame(0);

        // Set color for this timezone
        sprite.setTextColor(timeColor, clockBackgroundColor);
        sprite.drawNumber(dig->NewValue(), 0, 0);
        sprite.pushSprite(dig->X(), dig->Y());
        CLOCK_DEBUG_PRINTLN("Drew digit " + String(di) + " value " + String(dig->NewValue()) + " at (" + String(dig->X()) + "," + String(dig->Y()) + ")");
    }
    
    // Draw colon with day/night color - use the calculated position
    tft.setTextFont(clockFont);
    tft.setTextSize(clockSize);
    tft.setTextDatum(clockDatum);
    tft.setTextColor(timeColor, clockBackgroundColor);
    
    // Get the Y position from the first digit (they're all aligned)
    int colonY = digs[0]->Y();
    tft.drawChar(':', colons[0], colonY);

    // In 12-hour mode, show an AM/PM indicator in the top-right of the quadrant
    // (ispm was set by ParseDigits above).
    if (!SHOW_24HOUR) {
        QuadrantPos ampmQuad = quadrants[quadrantIndex];
        tft.setTextFont(1);
        tft.setTextSize(1);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(timeColor, clockBackgroundColor);
        tft.drawString(ispm ? "PM" : "AM", ampmQuad.x + quadrantWidth - 4, ampmQuad.y + 6);
    }

    // Draw date and day name below the time
    DrawDateAndDay(zone, quadrantIndex);
}

void showWiFiStatus(String message, uint16_t color = TFT_WHITE, int fontsize = 1)
{
    tft.fillScreen(clockBackgroundColor);
    tft.setTextFont(1);
    tft.setTextSize(fontsize);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color, clockBackgroundColor);
    tft.drawString(message, 160, 120);
    delay(100); // Brief pause so the status message is legible
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
    
    // Calculate fill width based on brightness (5-255 range)
    int fillWidth = map(brightness, 5, 255, 0, barWidth);
    
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
    int percentage = map(brightness, 5, 255, 0, 100);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(String(percentage) + "%", 160, barY + barHeight + 10);
}

void rollingClockSetup(bool is24Hour, bool usDate)
{
    Serial.println("World Clock Setup");
    SHOW_24HOUR = is24Hour;
    // usDate == true  -> MM/DD/YY (US),  usDate == false -> DD/MM/YY (rest of world)
    NOT_US_DATE = !usDate;
    SetupCYD();
    
    // Show WiFi connection status
    showWiFiStatus("Connecting WiFi...", TFT_YELLOW);
    
    SetupDigits();
    
    // Initialize touch screen
    touchscreen.begin();
    Serial.println("Touch screen initialized");
    
    // Initialize backlight pin with PWM
    pinMode(BACKLIGHT_PIN, OUTPUT);
    analogWrite(BACKLIGHT_PIN, backlightLevel);
    
    // Show WiFi connected status
    showWiFiStatus("WiFi Connected!", TFT_GREEN);
    
    // Show the web config URL on screen so the user can note it down
    String ipAddress = WiFi.localIP().toString();
    showWiFiStatus("Config: http://" + ipAddress, TFT_CYAN, 2);
    delay(1500);

    // Build the zones from saved/edited configuration
    initWorldZonesFromConfig();

    // Show timezone setup status
    showWiFiStatus("Setting up zones...", TFT_CYAN);

    // Initialize all timezones with retry mechanism
    for (int i = 0; i < 4; i++) {
        bool tzSuccess = false;
        int retryCount = 0;
        const int maxRetries = 5;
        
        while (!tzSuccess && retryCount < maxRetries) {
            // Show progress on screen
            String statusMsg = "Setting up ";
            statusMsg += worldZones[i].name;
            statusMsg += " (" + String(retryCount + 1) + "/" + String(maxRetries) + ")";
            showWiFiStatus(statusMsg, TFT_CYAN);
            
            Serial.print("Setting timezone ");
            Serial.print(worldZones[i].name);
            Serial.print(" (attempt ");
            Serial.print(retryCount + 1);
            Serial.println(")");
            
            // Try to set the timezone
            if (worldZones[i].tz.setLocation(worldZones[i].timezone)) {
                // // Wait a moment for timezone to stabilize
                // delay(500);
                
                // Verify the timezone was set correctly
                time_t local = worldZones[i].tz.now();
                time_t utc = UTC.now();
                
                // Check if local time is different from UTC (indicating timezone worked)
                if (abs((long)(local - utc)) > 60) { // More than 1 minute difference
                    tzSuccess = true;
                    
                    // Show success on screen
                    String successMsg = worldZones[i].name + " - OK!";
                    showWiFiStatus(successMsg, TFT_GREEN);
                    
                    Serial.print("SUCCESS: ");
                    Serial.print(worldZones[i].name);
                    Serial.print(" - ");
                    Serial.print(worldZones[i].timezone);
                    Serial.print(" | Time: ");
                    Serial.print(hour(local));
                    Serial.print(":");
                    if (minute(local) < 10) Serial.print("0");
                    Serial.println(minute(local));
                } else {
                    Serial.print("FAILED: Timezone not applied correctly (time matches UTC)");
                    Serial.println();
                }
            } else {
                Serial.println("FAILED: setLocation returned false");
            }
            
            if (!tzSuccess) {
                retryCount++;
                if (retryCount < maxRetries) {
                    // Show retry message on screen
                    showWiFiStatus("Retrying...", TFT_YELLOW);
                    Serial.println("Retrying in 2 seconds...");
                    delay(1000); // Additional delay since showWiFiStatus has its own delay
                }
            }
        }
        
        if (!tzSuccess) {
            // Show error on screen
            String errorMsg = worldZones[i].name + " - FAILED!";
            showWiFiStatus(errorMsg, TFT_RED);

            Serial.print("ERROR: Failed to set timezone for ");
            Serial.print(worldZones[i].name);
            Serial.println(" after all retries!");
        }

        // Seed the market-status cache so the first frame shows it immediately
        // (it is refreshed once per minute afterwards in hasTimeChanged).
        if (worldZones[i].market.hasMarket) {
            worldZones[i].lastMarketStatus = getMarketStatus(worldZones[i]);
        }
    }

    // Show ready status
    showWiFiStatus("World Clock Ready!", TFT_GREEN);
    
    // Show available serial commands and web interface info
    showStartupCommands();
    Serial.println("=== Web Configuration Available ===");
    Serial.println("Open http://" + WiFi.localIP().toString() + " in your browser");
    Serial.println("to configure timezones and market settings");
    Serial.println();
}

void handleTouch()
{
    static unsigned long lastTouchTime = 0;
    static unsigned long brightnessBarShownTime = 0;
    static bool brightnessBarVisible = false;
    
    TouchPoint touch = touchscreen.getTouch();
    
    // Check if screen is being touched (zRaw indicates pressure)
    if (touch.zRaw != 0) 
    {
        unsigned long currentTime = millis();
        
        // Debounce - only allow one touch every 50ms for brightness control
        if (currentTime - lastTouchTime > 50 && touch.zRaw > 800)
        {
            // Determine touch location (left vs right half of screen).
            // getTouch() already maps touch.x into screen pixels (0..screenWidth),
            // so the screen is 320px wide and we split at the 160px midpoint.
            if (touch.x < 160) // Left half - make dimmer
            {
                backlightLevel -= 5; // Decrease brightness
                if (backlightLevel <= 5) backlightLevel = 5; // Minimum brightness
                
                Serial.print("LEFT touch - Dimmer: ");
                Serial.println(backlightLevel);
            }
            else // Right half - make brighter
            {
                backlightLevel += 5; // Increase brightness
                if (backlightLevel > 255) backlightLevel = 255; // Maximum brightness
                
                Serial.print("RIGHT touch - Brighter: ");
                Serial.println(backlightLevel);
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
            
            Serial.print("Touch at X: ");
            Serial.print(touch.x);
            Serial.print(", Y: ");
            Serial.print(touch.y);
            Serial.print(", Pressure: ");
            Serial.print(touch.zRaw);
            Serial.print(", Brightness: ");
            Serial.println(backlightLevel);
        }
    }
    
    // Hide brightness bar after the configured timeout
    if (brightnessBarVisible && (millis() - brightnessBarShownTime > BRIGHTNESS_BAR_TIMEOUT_MS)) {
        brightnessBarVisible = false;
        // Clear the brightness bar area and force a full screen redraw
        tft.fillScreen(clockBackgroundColor);
        firstDraw = true; // This will force a complete redraw in the next cycle
        for (int i = 0; i < 4; i++) {
            worldZones[i].initialized = false;
        }
    }
}

// Small status dot at the centre of the screen (where the four quadrants meet):
//   green  = WiFi up and time synced
//   yellow = WiFi up but NTP needs a refresh
//   red    = WiFi down or time never set
void drawConnectionIndicator()
{
    uint16_t color;
    if (WiFi.status() != WL_CONNECTED) {
        color = TFT_RED;
    } else if (timeStatus() == timeNotSet) {
        color = TFT_RED;
    } else if (timeStatus() == timeNeedsSync) {
        color = TFT_YELLOW;
    } else {
        color = TFT_GREEN;
    }
    tft.fillCircle(160, 120, 3, color);
}

void drawRollingClock()
{
    // Handle serial commands
    handleSerialCommands();

    // Handle touch input for backlight control
    handleTouch();

    // Update flash state for market status messages
    updateFlashState();

    // Adjust brightness based on the home zone time
    adjustBrightnessBasedOnHomeTime();
    
    // Only clear screen and draw borders on first draw
    if (firstDraw) {
        tft.fillScreen(clockBackgroundColor);
        DrawQuadrantBorders();
        firstDraw = false;
        
        // Force redraw all quadrants on first run
        for (int i = 0; i < 4; i++) {
            DrawSingleTimeZone(worldZones[i], i, true);
        }
    } else {
        // Check each timezone for updates
        for (int i = 0; i < 4; i++) {
            if (hasTimeChanged(worldZones[i])) {
                // Full redraw needed (time, date, or market status changed)
                CLOCK_DEBUG_PRINTLN("Calling DrawSingleTimeZone for " + worldZones[i].name);
                DrawSingleTimeZone(worldZones[i], i, false);
            } else if (needsFlashOnlyUpdate(worldZones[i])) {
                // Only market status flashing update needed
                updateMarketStatusOnly(worldZones[i], i);
            }
        }
    }
    
    // Reset flash change flag after all zones have been processed
    resetFlashChangeFlag();

    // Draw the WiFi/NTP status dot on top of everything
    drawConnectionIndicator();
}