#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

// ---------------------------------------------------------------------------
// Current weather for the four configured zones, fetched from Open-Meteo
// (https://open-meteo.com - free, no API key) in a single multi-location
// request. A dedicated FreeRTOS task on core 0 does the fetching (every
// weatherRefreshMin minutes - web settings, default 20 - retrying 5 minutes
// after a failure), so the clock and touch UI on the main core never block.
// Used by the weather clock face.
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

// Current weather alert text for zone/quadrant i (0-3), or "" when none.
// US cities use the official US National Weather Service active-alerts feed
// (api.weather.gov); other cities derive a severe-condition alert from the WMO
// weather code (thunderstorm, heavy snow/rain, freezing rain). Refreshed by the
// background task on the same cadence as the weather. Always "" while the
// weatherAlerts setting is off. Uppercased and length-capped for the display.
String getZoneAlert(int i);

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

// Temperature ready for display: rounded, converted to Fahrenheit when the
// useFahrenheit setting is on (stored values stay Celsius). tempUnitLetter
// gives the matching 'C' / 'F' for faces that print the unit.
int displayTemp(float tempC);
char tempUnitLetter();

// Short display text / color for a WMO weather code, e.g. 61 -> "RAIN".
const char *weatherCodeText(int code);
uint16_t weatherCodeColor(int code);

#endif // WEATHER_SERVICE_H
