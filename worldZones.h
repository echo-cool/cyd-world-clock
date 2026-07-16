#ifndef WORLD_ZONES_H
#define WORLD_ZONES_H

// The world-clock zone model shared by every module: the TradingSession /
// MarketInfo / WorldClockZone structs and the four-quadrant worldZones array
// they describe (defined in ClockLogic.cpp). Split out of ClockLogic.h so the
// market/brightness/solar modules can share the types without a circular
// include back into the umbrella header.

#include <Arduino.h>
#include <ezTime.h>

// Minimum spacing between blocking ezTime setLocation() retries for a zone
// whose clock is still invalid (see hasTimeChanged).
const unsigned long TZ_REINIT_RETRY_MS = 60UL * 1000UL;

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
    String weatherAlert; // cached weather-alert text for this zone ("" = none)
    String weatherNotice; // cached rain/snow start-stop text for the date/weather row
    unsigned long lastTzReinitMs = 0; // last blocking setLocation() retry (0 = never)
};

// The four clock quadrants (defined and initialized in ClockLogic.cpp)
extern WorldClockZone worldZones[4];

#endif // WORLD_ZONES_H
