#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

// ---------------------------------------------------------------------------
// Current weather for the four configured zones, fetched from Open-Meteo
// (https://open-meteo.com - free, no API key) in a single multi-location
// request. A dedicated FreeRTOS task on core 0 does the fetching (every 20
// minutes, retrying 5 minutes after a failure), so the clock and touch UI on
// the main core never block. Used by the weather clock face.
// ---------------------------------------------------------------------------

#include <Arduino.h>

struct ZoneWeather
{
    bool valid;
    float tempC;
    int weatherCode; // WMO weather interpretation code from Open-Meteo
};

// Start the background fetch task. Call once from setup, after the zones have
// been configured (applyConfiguredZones) and WiFi is up.
void weatherBegin();

// Latest weather for zone/quadrant i (0-3). .valid is false until a fetch has
// succeeded for that zone (or if the zone's timezone has no known city
// coordinates - see getCityCoords in uiPages).
ZoneWeather getZoneWeather(int i);

// Age of the current data in minutes, or -1 if no successful fetch yet.
long weatherAgeMinutes();

// Monotonic counter bumped whenever the stored data changes; the weather face
// compares it between frames so fresh data repaints without waiting for the
// next minute tick.
uint32_t weatherDataVersion();

// Drop cached data, re-snapshot the zones' city coordinates and refetch
// promptly. MAIN core only (reads the worldZones Strings) - call it when a
// zone's timezone/city changes.
void weatherInvalidate();

// Short display text / color for a WMO weather code, e.g. 61 -> "RAIN".
const char *weatherCodeText(int code);
uint16_t weatherCodeColor(int code);

#endif // WEATHER_SERVICE_H
