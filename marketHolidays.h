#ifndef MARKET_HOLIDAYS_H
#define MARKET_HOLIDAYS_H

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Exchange closure calendars: full-day holidays and half-day early closes.
//
// Two layers:
//  - Compiled-in tables (marketHolidays.cpp) - always available, but frozen
//    at build time, so they age out as new years are announced.
//  - A remote override, fetched weekly as JSON from MARKET_HOLIDAYS_URL
//    (marketHolidays.json in this repository by default - see that file for
//    the format) and cached in SPIFFS so a reboot doesn't need the network.
//    Exchanges present in the fetched data replace their compiled table
//    (holidays AND early closes); exchanges absent from it keep the
//    compiled fallback for both.
//
// The URL can be overridden by defining MARKET_HOLIDAYS_URL in secrets.h.
//
// Threading: the blocking HTTPS fetch runs on the core-0 background task
// (marketHolidaysTick, called from the weather task); scheduling decisions
// that need ezTime run on the main core (marketHolidaysService). The tables
// themselves are mutex-guarded, so the lookups are safe from either core.
// ---------------------------------------------------------------------------

// True if y/m/d (the exchange's local date) is a full-day closure for the
// given exchange ("NYSE", "LSE", "SSE", "TSE", "HKEX"). Dates outside the
// tables' horizon return false, i.e. the market status degrades to plain
// weekend-only awareness.
bool isMarketHoliday(const String &exchange, int y, int m, int d);

// Minutes-since-midnight (exchange-local) at which the exchange closes early
// on y/m/d (e.g. 780 for a 1:00 PM NYSE half day), or -1 for a normal day.
// getMarketStatus truncates the trading sessions at this time.
int marketEarlyCloseMinutes(const String &exchange, int y, int m, int d);

// Load the cached holiday file from SPIFFS and create the lock. Call once
// from setup after SPIFFS.begin(), before any other core touches the tables.
void marketHolidaysBegin();

// MAIN core, every loop: decides (at most once a minute, using ezTime) when
// a refresh is due and flags it for the background task.
void marketHolidaysService();

// Core-0 background task (shares the weather task): performs the HTTPS fetch
// when one has been flagged. No ezTime access.
void marketHolidaysTick();

// Status-page summary: true when the weekly-fetched calendars are active
// (ageDays filled with the age of the last good fetch, -1 if unknown),
// false when running on the compiled-in tables. MAIN core only (ezTime).
bool marketHolidaysFetchedInfo(long &ageDays);

// Serial command support: dump the table state, and queue an immediate fetch.
void printMarketHolidaysStatus();
void marketHolidaysForceRefresh();

#endif // MARKET_HOLIDAYS_H
