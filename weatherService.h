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
    int weatherCode;   // WMO weather interpretation code from Open-Meteo
    bool hasMinMax;    // today's forecast high/low parsed OK
    float tempMaxC;    // today's forecast high (local day, timezone=auto)
    float tempMinC;    // today's forecast low
    int humidity;      // current relative humidity %, -1 when missing
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

// True when zone i's current alert came from the official NWS feed rather
// than being derived from the weather code. The big-clock face lets only
// official alerts outrank its precipitation chart - a derived STORM alert
// repeats what the chart's orange bars already show.
bool getZoneAlertOfficial(int i);

// Short near-term precipitation transition for the quadrant date/weather line,
// e.g. "RAIN IN 30M" or "RAIN STOPS IN 2H". Derived from Open-Meteo's
// 15-minute weather-code forecast for the next few hours.
String getZonePrecipNotice(int i);

// 15-minute precipitation forecast for zone i, starting at the current
// 15-minute block: amounts in mm per step into mm[], the matching WMO
// weather codes into codes[] (for coloring rain/snow/storm bars). Returns
// the number of steps copied (up to maxOut / PRECIP_FORECAST_STEPS), 0
// until a fetch has succeeded. Feeds the big-clock face's rain chart.
const int PRECIP_FORECAST_STEPS = 16; // current + 3h45m at 15-min cadence
int getZonePrecip15(int i, float *mm, uint8_t *codes, int maxOut);

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
bool weatherCodeHasPrecip(int code);

#endif // WEATHER_SERVICE_H
