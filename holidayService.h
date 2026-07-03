#ifndef HOLIDAY_SERVICE_H
#define HOLIDAY_SERVICE_H

// ---------------------------------------------------------------------------
// Named public holidays for the four configured zones, from the free Nager
// API (https://date.nager.at - no key needed). Each zone's country gets its
// current local year fetched once, then refreshed weekly, on a zone change
// and at the year rollover. The blocking HTTPS work runs on the core-0
// background task (holidaysTick, shared with the weather fetcher); the
// tables are mutex-guarded so the render code can query them any time.
//
// Zones whose preset has no country code (Dubai and Mumbai - not covered by
// the API) simply never report a holiday, same as weather degrades for
// cities without coordinates.
// ---------------------------------------------------------------------------

#include <Arduino.h>

// One public holiday: date as YYYYMMDD plus the uppercased English name.
struct PublicHoliday
{
    uint32_t date;
    char name[32];
};

// Create the lock and snapshot the zones' countries. Call once from setup,
// after applyConfiguredZones and before the core-0 task starts.
void holidaysBegin();

// MAIN core, every loop: tracks each zone's current local year (needs
// ezTime) and flags stale data for refresh. Cheap - self-gates to once a
// minute.
void holidaysService();

// Core-0 background task: performs one country-year fetch per call when
// something is missing or stale. No ezTime access.
void holidaysTick();

// MAIN core: the zones changed - re-snapshot countries and refetch what
// no longer matches. Data for unchanged countries is kept.
void holidaysInvalidate();

// True (and the name copied out) if ymd (YYYYMMDD, the zone's local date) is
// a public holiday for zone/quadrant i (0-3).
bool getHolidayName(int zoneIdx, uint32_t ymd, char *out, size_t outLen);

// Copy zone i's holiday list (date-ascending) into out; returns the count.
int getZoneHolidays(int zoneIdx, PublicHoliday *out, int maxOut);

// Monotonic counter bumped whenever the stored data changes; faces compare
// it between frames to repaint as soon as an async fetch lands.
uint32_t holidaysDataVersion();

// Serial command support (part of the HOLIDAYS command output).
void printPublicHolidaysStatus();

#endif // HOLIDAY_SERVICE_H
