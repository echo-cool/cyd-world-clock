#ifndef MARKET_STATUS_H
#define MARKET_STATUS_H

// Market/trading-session status for the world-clock zones: the status line
// text ("NYSE OPEN", open/close countdowns), its display color, and the
// regular-session progress/close queries. Moved out of ClockLogic.cpp/.h;
// the session tables live in each zone's MarketInfo (worldZones.h), the
// holiday calendars in marketHolidays.cpp and the session-membership math in
// marketSession.cpp.

#include <Arduino.h>

#include "worldZones.h"

const int MARKET_STATUS_MESSAGE_MIN = 10;

// Current market status line for a zone, e.g. "NYSE OPEN" (heavy String work -
// callers outside ClockLogic.cpp should prefer zone.lastMarketStatus, which is
// refreshed once per minute).
String getMarketStatus(WorldClockZone &zone);

// Display color for a market status string ("NYSE OPEN" -> green, ...).
uint16_t getMarketStatusColor(String status);

// True for the time-sensitive <=10-minute open/close market alerts, which
// blink on the quadrant status line.
bool shouldMessageFlash(String message);

// Fraction (0..1) of the exchange's regular trading day already elapsed;
// false outside regular hours (weekend, holiday, before open / after close).
bool marketDayProgress(WorldClockZone &zone, float &frac);

// Minutes until the REGULAR session currently in progress closes (half-day
// early closes applied); -1 when no regular session is running.
long marketMinutesToRegularClose(WorldClockZone &zone);

#endif // MARKET_STATUS_H
