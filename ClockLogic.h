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

int MARKET_STATUS_MESSAGE_MIN = 10;

XPT2046_Bitbang touchscreen(MOSI_PIN, MISO_PIN, CLK_PIN, CS_PIN);

int clockFont = 4;  // Changed to font 4 for better readability 
int clockSize = 2;  // Moderate size for Font 4 to fit in quadrants
int clockDatum = TL_DATUM;
uint16_t clockBackgroundColor = TFT_BLACK;
uint16_t clockFontColor = TFT_WHITE;  // Changed to white for better contrast
int prevDay = 0;

bool SHOW_24HOUR = true;
bool SHOW_AMPM = false;

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
    {"LONDON", "Europe/London", Timezone(), -1, -1, false, -1, false, 
     {"LSE", true, {
         {7, 15, 8, 0, "PRE-MARKET"},    // Pre-market trading (order input)
         {8, 0, 16, 30, "REGULAR"},      // Regular trading hours
         {16, 30, 17, 0, "CLOSING"},     // Closing auction period
         {17, 0, 17, 30, "AFTER-HRS"}    // After-hours reporting/settlement
     }, 4}, ""}
};

// Include serial commands after WorldClockZone is defined
#include "serialCommands.h"

// Global variables for touch and backlight control
bool firstDraw = true;
bool backlightOn = true;
int backlightLevel = 80; // PWM value (0-255)
uint16_t touchX, touchY;

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

int ampm[2]; // X, Y of the AM or PM indicator
bool ispm;

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
    int hr = hour(local); // Always use 24-hour format
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
    
    // Weekend logic with special cases for Sunday evening and Friday evening
    if (currentDayOfWeek == 7) { // Saturday
        // Saturday is generally closed for all markets
        return zone.market.exchange + " CLOSED";
    } else if (currentDayOfWeek == 1) { // Sunday
        // Sunday evening trading - check if any sessions are active
        // US markets may have overnight sessions continuing from Friday or starting Sunday evening
        if (zone.market.exchange == "NYSE") {
            // Check if we're in an overnight session that started Friday night
            for (int i = 0; i < zone.market.sessionCount; i++) {
                TradingSession session = zone.market.sessions[i];
                if (session.sessionName == "OVERNIGHT" && session.sessionName.length() > 0) {
                    int sessionStart = session.openHour * 60 + session.openMinute;
                    int sessionEnd = session.closeHour * 60 + session.closeMinute;
                    
                    // For overnight sessions that span from Friday to Sunday
                    if (sessionEnd < sessionStart && currentTotalMinutes < sessionEnd) {
                        int minutesToClose = sessionEnd - currentTotalMinutes;
                        if (minutesToClose <= MARKET_STATUS_MESSAGE_MIN) {
                            return zone.market.exchange + " " + session.sessionName + " CLOSE IN " + String(minutesToClose) + "MIN";
                        }
                        return zone.market.exchange + " " + session.sessionName + " OPEN";
                    }
                }
                
                // Check for Sunday evening trading (futures/forex start around 6 PM ET Sunday)
                if (session.sessionName == "OVERNIGHT" && currentTotalMinutes >= 18 * 60) { // After 6 PM Sunday
                    // Sunday 6 PM ET is when futures markets typically open for the week
                    int minutesToClose = session.closeHour * 60 + session.closeMinute; // Monday 4 AM
                    if (minutesToClose <= MARKET_STATUS_MESSAGE_MIN && currentTotalMinutes >= 18 * 60) {
                        return zone.market.exchange + " CLOSE IN " + String(minutesToClose) + "MIN";
                    }
                    return zone.market.exchange + " OPEN";
                }
                
                // Also check if pre-market is starting late Sunday (some brokers start at 1 AM Monday)
                if (session.sessionName == "PRE-MARKET" && currentTotalMinutes >= 1 * 60) { // After 1 AM Sunday night
                    int sessionStart = session.openHour * 60 + session.openMinute;
                    if (currentTotalMinutes >= sessionStart) {
                        return zone.market.exchange + " " + session.sessionName + " OPEN";
                    } else if (sessionStart - currentTotalMinutes <= MARKET_STATUS_MESSAGE_MIN) {
                        return zone.market.exchange + " " + session.sessionName + " OPEN IN " + String(sessionStart - currentTotalMinutes) + "MIN";
                    }
                }
            }
        }
        return zone.market.exchange + " CLOSED";
    } else if (currentDayOfWeek == 6) { // Friday
        // Friday may have extended evening hours - check if overnight sessions extend into weekend
        if (zone.market.exchange == "NYSE") {
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
        
        // Handle sessions that span midnight (like overnight trading)
        bool isCurrentlyInSession = false;
        if (sessionEnd < sessionStart) {
            // Session spans midnight (e.g., 20:00 to 04:00)
            isCurrentlyInSession = (currentTotalMinutes >= sessionStart || currentTotalMinutes < sessionEnd);
        } else {
            // Normal session within same day
            isCurrentlyInSession = (currentTotalMinutes >= sessionStart && currentTotalMinutes < sessionEnd);
        }
        
        if (isCurrentlyInSession) {
            // Currently in this session - check if closing soon
            int minutesToClose;
            if (sessionEnd < sessionStart && currentTotalMinutes >= sessionStart) {
                // We're in the first part of a midnight-spanning session
                minutesToClose = (24 * 60) - currentTotalMinutes + sessionEnd;
            } else {
                minutesToClose = sessionEnd - currentTotalMinutes;
            }
            
            if (minutesToClose <= MARKET_STATUS_MESSAGE_MIN) {
                if (session.sessionName == "REGULAR") {
                    return zone.market.exchange + " CLOSE IN " + String(minutesToClose) + "MIN";
                } else {
                    return zone.market.exchange + " " + session.sessionName + " CLOSE IN " + String(minutesToClose) + "MIN";
                }
            }
            
            if (session.sessionName == "REGULAR") {
                return zone.market.exchange + " OPEN";
            } else {
                return zone.market.exchange + " " + session.sessionName + " OPEN";
            }
        }
        
        // Check if next session is opening soon
        int minutesToOpen;
        if (sessionEnd < sessionStart) {
            // Next session spans midnight
            if (currentTotalMinutes < sessionStart) {
                minutesToOpen = sessionStart - currentTotalMinutes;
            } else {
                minutesToOpen = (24 * 60) - currentTotalMinutes + sessionStart;
            }
        } else {
            // Normal next session
            if (currentTotalMinutes < sessionStart) {
                minutesToOpen = sessionStart - currentTotalMinutes;
            } else {
                // Check next day's first session
                continue;
            }
        }
        
        if (minutesToOpen <= MARKET_STATUS_MESSAGE_MIN) {
            if (session.sessionName == "REGULAR") {
                return zone.market.exchange + " OPEN IN " + String(minutesToOpen) + "MIN";
            } else {
                return zone.market.exchange + " " + session.sessionName + " OPEN IN " + String(minutesToOpen) + "MIN";
            }
        }
    }
    
    return zone.market.exchange + " CLOSED";
}

uint16_t getMarketStatusColor(String status)
{
    // Check for specific session types first (before generic OPEN check)
    if (status.indexOf("AFTER-HRS OPEN") != -1 || status.indexOf("MORNING OPEN") != -1 || status.indexOf("AFTERNOON OPEN") != -1) {
        return TFT_CYAN;    // Cyan for extended/session trading
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
    
    // For all timezones, add relative date indication compared to home (top-left)
    String dayText = dayNames[dow];
    time_t homeTime = worldZones[0].tz.now();
    int homeDay = day(homeTime);
    
    if (dd > homeDay || (dd == 1 && homeDay > 20)) { // Tomorrow relative to home (or next month)
        dayText += " (+1)";
    } else if (dd < homeDay || (dd > 20 && homeDay == 1)) { // Yesterday relative to home (or prev month)  
        dayText += " (-1)";
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
    
    // Draw market status if this zone has a market
    String marketStatus = getMarketStatus(zone);
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
    // Draw grid lines to separate quadrants with more visible color
    uint16_t gridColor = TFT_WHITE;  // Changed to white for better visibility
    
    // // Main grid lines (thicker and more visible)
    // for (int i = 158; i <= 162; i++) {
    //     tft.drawLine(i, 0, i, 240, gridColor);   // Thicker vertical center line
    // }
    
    // for (int i = 118; i <= 122; i++) {
    //     tft.drawLine(0, i, 320, i, gridColor);   // Thicker horizontal center line
    // }
    
    // // Outer border frame (thicker)
    // for (int i = 0; i < 3; i++) {
    //     tft.drawRect(i, i, 320-i*2, 240-i*2, gridColor);     // Thick outer border
    // }
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
    String marketStatus = getMarketStatus(zone);
    
    if (marketStatus.length() > 0 && shouldMessageFlash(marketStatus)) {
        // Calculate the exact area where market status is displayed
        tft.setTextFont(1);
        tft.setTextSize(1);
        int textWidth = tft.textWidth(marketStatus);
        int textHeight = tft.fontHeight();
        int textX = quad.centerX - textWidth / 2;
        int textY = quad.y + quadrantHeight - 10;
        
        // Clear only the market status area
        tft.fillRect(textX - 2, textY - 1, textWidth + 4, textHeight + 2, clockBackgroundColor);
        
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
    int currentHour = hour(local); // Always use 24-hour format
    int currentMinute = minute(local);
    int currentDay = day(local);
    
    // Check if market status has actually changed
    bool marketStatusChanged = false;
    if (zone.market.hasMarket) {
        String currentMarketStatus = getMarketStatus(zone);
        if (currentMarketStatus != zone.lastMarketStatus) {
            zone.lastMarketStatus = currentMarketStatus;
            marketStatusChanged = true;
        }
    }
    
    if (!zone.initialized || 
        zone.lastHour != currentHour || 
        zone.lastMinute != currentMinute || 
        zone.lastDay != currentDay ||
        marketStatusChanged)
    {
        zone.lastHour = currentHour;
        zone.lastMinute = currentMinute;
        zone.lastDay = currentDay;
        zone.initialized = true;
        return true;
    }
    return false;
}

bool needsFlashOnlyUpdate(WorldClockZone &zone)
{
    if (flashJustChanged && zone.market.hasMarket) {
        String status = getMarketStatus(zone);
        return shouldMessageFlash(status);
    }
    return false;
}

void adjustBrightnessBasedOnHomeTime()
{
    static int lastHourChecked = -1;
    static unsigned long lastBrightnessAdjustment = 0;
    
    // Only check brightness adjustment every 30 seconds to avoid excessive updates
    unsigned long currentTime = millis();
    if (currentTime - lastBrightnessAdjustment < 30000) {
        return;
    }
    
    // Get current hour from Santa Clara (home location - worldZones[0])
    if (worldZones[0].initialized) {
        time_t santaClaraTime = worldZones[0].tz.now();
        int currentHour = hour(santaClaraTime);
        
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
                analogWrite(21, backlightLevel);
                
                Serial.print("Auto brightness adjusted for Santa Clara time ");
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
    // Clear only this quadrant
    tft.fillRect(quad.x, quad.y, quadrantWidth, quadrantHeight, clockBackgroundColor);
    
    // Redraw grid lines that intersect this quadrant
    uint16_t gridColor = TFT_WHITE;
    
    // // Redraw vertical grid line if this quadrant is on the left
    // if (quadrantIndex == 0 || quadrantIndex == 2) { // Left quadrants
    //     for (int i = 158; i <= 162; i++) {
    //         tft.drawLine(i, quad.y, i, quad.y + quadrantHeight, gridColor);
    //     }
    // }
    
    // // Redraw horizontal grid line if this quadrant is on top
    // if (quadrantIndex == 0 || quadrantIndex == 1) { // Top quadrants
    //     for (int i = 118; i <= 122; i++) {
    //         tft.drawLine(quad.x, i, quad.x + quadrantWidth, i, gridColor);
    //     }
    // }
    
    // // Always redraw the outer borders for this quadrant
    // if (quadrantIndex == 0) { // Top-left
    //     for (int i = 0; i < 3; i++) {
    //         tft.drawLine(i, quad.y, i, quad.y + quadrantHeight, gridColor); // Left border
    //         tft.drawLine(quad.x, i, quad.x + quadrantWidth, i, gridColor);   // Top border
    //     }
    // }
    // if (quadrantIndex == 1) { // Top-right
    //     for (int i = 0; i < 3; i++) {
    //         tft.drawLine(319-i, quad.y, 319-i, quad.y + quadrantHeight, gridColor); // Right border
    //         tft.drawLine(quad.x, i, quad.x + quadrantWidth, i, gridColor);          // Top border
    //     }
    // }
    // if (quadrantIndex == 2) { // Bottom-left
    //     for (int i = 0; i < 3; i++) {
    //         tft.drawLine(i, quad.y, i, quad.y + quadrantHeight, gridColor);         // Left border
    //         tft.drawLine(quad.x, 239-i, quad.x + quadrantWidth, 239-i, gridColor); // Bottom border
    //     }
    // }
    // if (quadrantIndex == 3) { // Bottom-right
    //     for (int i = 0; i < 3; i++) {
    //         tft.drawLine(319-i, quad.y, 319-i, quad.y + quadrantHeight, gridColor); // Right border
    //         tft.drawLine(quad.x, 239-i, quad.x + quadrantWidth, 239-i, gridColor); // Bottom border
    //     }
    // }
}

void DrawSingleTimeZone(WorldClockZone &zone, int quadrantIndex, bool forceRedraw = false)
{
    // Check if we need to update this timezone
    bool needsUpdate = forceRedraw || hasTimeChanged(zone);
    
    if (!needsUpdate) {
        return; // No change, skip drawing
    }
    
    // Clear only this quadrant to avoid flashing
    DrawQuadrantBackground(quadrantIndex);
    
    // Calculate digit positions for this quadrant
    CalculateDigitOffsetsForQuadrant(quadrantIndex);
    
    // Parse time for this timezone
    ParseDigits(zone.tz);
    
    // Get day/night appropriate colors
    uint16_t timeColor = getDayNightColor(zone.tz);
    
    // Draw timezone label at top of quadrant
    DrawTimeZoneLabel(zone.name, quadrantIndex);
    
    // Draw time digits without animation for smooth display
    for (size_t di = 0; di < 4; di++)
    {
        Digit *dig = digs[di];
        dig->Value(dig->NewValue());
        dig->Frame(0);
        
        // Set color for this timezone
        sprite.setTextColor(timeColor, clockBackgroundColor);
        sprite.drawNumber(dig->NewValue(), 0, 0);
        sprite.pushSprite(dig->X(), dig->Y());
    }
    
    // Draw colon with day/night color - use the calculated position
    tft.setTextFont(clockFont);
    tft.setTextSize(clockSize);
    tft.setTextDatum(clockDatum);
    tft.setTextColor(timeColor, clockBackgroundColor);
    
    // Get the Y position from the first digit (they're all aligned)
    int colonY = digs[0]->Y();
    tft.drawChar(':', colons[0], colonY);
    
    // Draw date and day name below the time
    DrawDateAndDay(zone, quadrantIndex);
}

void showWiFiStatus(String message, uint16_t color = TFT_WHITE)
{
    tft.fillScreen(clockBackgroundColor);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color, clockBackgroundColor);
    tft.drawString(message, 160, 120);
    delay(100); // Show message for 1 second
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

void rollingClockSetup(bool is24Hour, bool notUsDate)
{
    Serial.println("World Clock Setup");
    SHOW_24HOUR = is24Hour;
    SHOW_AMPM = !is24Hour;
    NOT_US_DATE = notUsDate;
    SetupCYD();
    
    // Show WiFi connection status
    showWiFiStatus("Connecting WiFi...", TFT_YELLOW);
    
    SetupDigits();
    
    // Initialize touch screen
    touchscreen.begin();
    Serial.println("Touch screen initialized");
    
    // Initialize backlight pin with PWM
    pinMode(21, OUTPUT);
    analogWrite(21, backlightLevel);
    
    // Show WiFi connected status
    showWiFiStatus("WiFi Connected!", TFT_GREEN);
    
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
    }
    
    // Show ready status
    showWiFiStatus("World Clock Ready!", TFT_GREEN);
    
    // Show available serial commands
    showStartupCommands();
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
            // Determine touch location (left vs right half of screen)
            // Screen is 320px wide, so divide at 160px
            if (touch.x < 160) // Left half - make dimmer (touch coordinates are usually 0-4095)
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
            analogWrite(21, backlightLevel);
            
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
    
    // Hide brightness bar after 2 seconds
    if (brightnessBarVisible && (millis() - brightnessBarShownTime > 1000)) {
        brightnessBarVisible = false;
        // Clear the brightness bar area and force a full screen redraw
        tft.fillScreen(clockBackgroundColor);
        firstDraw = true; // This will force a complete redraw in the next cycle
        for (int i = 0; i < 4; i++) {
            worldZones[i].initialized = false;
        }
    }
}

void drawRollingClock()
{
    // Handle serial commands
    handleSerialCommands();
    
    // Handle touch input for backlight control
    handleTouch();
    
    // Update flash state for market status messages
    updateFlashState();
    
    // Adjust brightness based on Santa Clara time
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
                DrawSingleTimeZone(worldZones[i], i, false);
            } else if (needsFlashOnlyUpdate(worldZones[i])) {
                // Only market status flashing update needed
                updateMarketStatusOnly(worldZones[i], i);
            }
        }
    }
    
    // Reset flash change flag after all zones have been processed
    resetFlashChangeFlag();
}