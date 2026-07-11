#include "weatherService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "ClockLogic.h"     // worldZones
#include "holidayService.h" // holidaysTick - shares this task's HTTPS stack
#include "marketHolidays.h" // marketHolidaysTick - shares this task's HTTPS stack
#include "netCheck.h"       // netCheckService - captive-portal re-check
#include "otaUpdate.h"      // otaInProgress - pause fetching during an update
#include "projectConfig.h"  // weatherAlerts / useFahrenheit / weatherRefreshMin
#include "uiPages.h"        // getCityCoords, getCountryForTimezone
#include "wifiRelay.h"      // wifiRelayActive - the relay does its own probing

// Success-path refresh interval comes from projectConfig.weatherRefreshMin
// (web settings page, default 20 minutes, re-read every due-check).
const unsigned long WEATHER_RETRY_MS = 5UL * 60UL * 1000UL; // after a failure
const unsigned long WEATHER_TASK_TICK_MS = 2000;            // due-check cadence
// The 15-minute forecast window (PRECIP_FORECAST_STEPS, weatherService.h)
// feeds both the precip-transition notices and the big-clock rain chart.

static unsigned long weatherRefreshMs()
{
    return (unsigned long)constrain(projectConfig.weatherRefreshMin, 5, 120) * 60000UL;
}

struct RenderBufferReleaseGuard
{
    bool released;
    RenderBufferReleaseGuard() : released(clockReleaseRenderBufferForNetwork()) {}
    ~RenderBufferReleaseGuard() { clockRestoreRenderBufferForNetwork(released); }
};

// City coordinates the fetch task works from. Snapshotted from worldZones on
// the MAIN core (weatherBegin / weatherInvalidate), so the task never reads
// the zones' Strings while the touch UI might be reassigning them.
struct WeatherLoc
{
    bool has;
    bool isUS; // US cities use the NWS alerts feed; others derive from the code
    float lat;
    float lon;
};

// Everything below is shared between the main core (readers, invalidate) and
// the fetch task on core 0 (writer); guard every access with weatherMutex.
static SemaphoreHandle_t weatherMutex = nullptr;
static WeatherLoc weatherLocs[4] = {};
static uint32_t locsGeneration = 0; // bumped on snapshot; stale fetches discard
static ZoneWeather zoneWeather[4] = {};
static String zoneAlert[4];         // weather alert text per zone ("" = none)
static bool zoneAlertOfficial[4] = {}; // alert came from the NWS feed
static String zonePrecipNotice[4];  // short rain/snow start-stop text
static float zonePrecipMm[4][PRECIP_FORECAST_STEPS] = {};    // mm per 15-min step
static uint8_t zonePrecipCode[4][PRECIP_FORECAST_STEPS] = {}; // WMO code per step
static int zonePrecipCount[4] = {};
static uint32_t dataVersion = 0;
static bool fetchForced = true; // fetch as soon as the task starts
static bool attempted = false;
static bool succeeded = false;
static unsigned long lastAttemptMillis = 0;
static unsigned long lastSuccessMillis = 0;

static void weatherLock()
{
    if (weatherMutex) xSemaphoreTake(weatherMutex, portMAX_DELAY);
}

static void weatherUnlock()
{
    if (weatherMutex) xSemaphoreGive(weatherMutex);
}

// Refresh weatherLocs from the configured zones. MAIN core only (reads the
// worldZones Strings); callers hold the mutex.
static void snapshotLocationsLocked()
{
    for (int i = 0; i < 4; i++)
    {
        float lat = 0, lon = 0;
        weatherLocs[i].has = getCityCoords(worldZones[i].timezone, lat, lon);
        weatherLocs[i].lat = lat;
        weatherLocs[i].lon = lon;
        const char *country = getCountryForTimezone(worldZones[i].timezone);
        weatherLocs[i].isUS = (country && strcmp(country, "US") == 0);
    }
    locsGeneration++;
}

ZoneWeather getZoneWeather(int i)
{
    if (i < 0 || i > 3) return ZoneWeather{};
    weatherLock();
    ZoneWeather w = zoneWeather[i];
    weatherUnlock();
    return w;
}

String getZoneAlert(int i)
{
    if (i < 0 || i > 3) return "";
    weatherLock();
    String a = zoneAlert[i];
    weatherUnlock();
    return a;
}

bool getZoneAlertOfficial(int i)
{
    if (i < 0 || i > 3) return false;
    weatherLock();
    bool o = zoneAlertOfficial[i];
    weatherUnlock();
    return o;
}

String getZonePrecipNotice(int i)
{
    if (i < 0 || i > 3) return "";
    weatherLock();
    String n = zonePrecipNotice[i];
    weatherUnlock();
    return n;
}

int getZonePrecip15(int i, float *mm, uint8_t *codes, int maxOut)
{
    if (i < 0 || i > 3 || maxOut <= 0) return 0;
    weatherLock();
    int n = min(zonePrecipCount[i], maxOut);
    for (int k = 0; k < n; k++)
    {
        mm[k] = zonePrecipMm[i][k];
        codes[k] = zonePrecipCode[i][k];
    }
    weatherUnlock();
    return n;
}

long weatherAgeMinutes()
{
    weatherLock();
    bool ok = succeeded;
    unsigned long t = lastSuccessMillis;
    weatherUnlock();
    if (!ok) return -1;
    return (long)((millis() - t) / 60000UL);
}

uint32_t weatherDataVersion()
{
    weatherLock();
    uint32_t v = dataVersion;
    weatherUnlock();
    return v;
}

void weatherInvalidate()
{
    weatherLock();
    snapshotLocationsLocked();
    for (int i = 0; i < 4; i++)
    {
        zoneWeather[i].valid = false;
        zoneAlert[i] = "";
        zoneAlertOfficial[i] = false;
        zonePrecipNotice[i] = "";
        zonePrecipCount[i] = 0;
    }
    attempted = false;
    succeeded = false;
    fetchForced = true;
    dataVersion++; // the face repaints its rows as "--" right away
    weatherUnlock();
}

// Severe-weather alert derived from a WMO code, for non-US cities (which have
// no NWS feed). "" for ordinary conditions - only genuinely rough weather is
// surfaced, to keep the weekday line uncluttered. Kept short for the 160px
// quadrant. WMO codes: https://open-meteo.com/en/docs
static const char *severeWeatherText(int code)
{
    switch (code)
    {
    case 65: return "HEAVY RAIN";       // heavy rain
    case 82: return "HEAVY SHOWERS";    // violent rain showers
    case 66:
    case 67: return "FREEZING RAIN";    // freezing rain
    case 75:                            // heavy snowfall
    case 86: return "HEAVY SNOW";       // heavy snow showers
    case 95: return "THUNDERSTORM";            // thunderstorm
    case 96:
    case 99: return "HAIL THUNDERSTORM";       // thunderstorm with hail
    default: return "";                 // not alert-worthy
    }
}

// Uppercase, abbreviate the one long common phrase, and cap the width so a
// long NWS event name still fits a quadrant.
static String normalizeAlertText(String s)
{
    s.toUpperCase();
    s.replace("THUNDERSTORM", "T-STORM");
    if (s.length() > 26) s = s.substring(0, 26);
    return s;
}

bool weatherCodeHasPrecip(int code)
{
    return (code >= 51 && code <= 67) || // drizzle, rain, freezing rain
           (code >= 71 && code <= 77) || // snow
           (code >= 80 && code <= 86) || // rain/snow showers
           (code >= 95 && code <= 99);   // thunderstorms
}

static const char *precipLabelForCode(int code)
{
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "SNOW";
    if (code >= 95) return "STORM";
    return "RAIN";
}

static String compactLeadTime(int minutes)
{
    if (minutes < 60) return String(minutes) + "M";
    int h = minutes / 60;
    int m = minutes % 60;
    if (m == 0) return String(h) + "H";
    return String(h) + "H" + String(m) + "M";
}

static String precipTransitionNotice(int currentCode, JsonArray forecastCodes)
{
    if (forecastCodes.isNull() || forecastCodes.size() < 2) return "";

    bool nowWet = weatherCodeHasPrecip(currentCode);
    if (nowWet)
    {
        for (int i = 1; i < (int)forecastCodes.size(); i++)
        {
            if (!weatherCodeHasPrecip(forecastCodes[i].as<int>()))
            {
                return String(precipLabelForCode(currentCode)) +
                       " STOPS IN " + compactLeadTime(i * 15);
            }
        }
        return "";
    }

    for (int i = 1; i < (int)forecastCodes.size(); i++)
    {
        int code = forecastCodes[i].as<int>();
        if (weatherCodeHasPrecip(code))
        {
            return String(precipLabelForCode(code)) +
                   " IN " + compactLeadTime(i * 15);
        }
    }
    return "";
}

// Rank an NWS severity string so the most serious active alert wins.
static int severityRank(const char *sev)
{
    if (!sev) return 0;
    if (strcmp(sev, "Extreme") == 0) return 3;
    if (strcmp(sev, "Severe") == 0) return 2;
    if (strcmp(sev, "Moderate") == 0) return 1;
    return 0; // Minor / Unknown
}

// Most severe active US National Weather Service alert for a point, or "".
// api.weather.gov is free/no-key but requires a descriptive User-Agent. A
// JSON filter keeps only each alert's event + severity, so the (potentially
// large) response parses in a few KB.
static String fetchUsAlert(float lat, float lon)
{
    RenderBufferReleaseGuard renderMemory;

    String url = "https://api.weather.gov/alerts/active?point=" +
                 String(lat, 4) + "," + String(lon, 4);

    WiFiClientSecure client;
    client.setInsecure(); // public alert data - certificate pinning not worth the upkeep
    HTTPClient http;
    // api.weather.gov presents a long RSA chain whose TLS handshake takes the
    // ESP32 several seconds of crypto - with the whole handshake inside the
    // connect window, 4s aborts it (-1) whenever the CPU is busy (always, at
    // boot). Open-meteo's small ECDSA chain never hit this.
    http.setConnectTimeout(12000);
    http.setTimeout(8000);
    if (!http.begin(client, url)) return "";
    http.addHeader("User-Agent",
                   "ESP32WorldClock (github.com/echo-cool/cyd-world-clock)");
    http.addHeader("Accept", "application/geo+json");

    int code = http.GET();
    if (code < 0)
    {
        // Connection-layer failure (TLS handshake starved of heap right after
        // boot is the usual culprit). One short-delay retry usually succeeds,
        // and beats sitting on a stale/absent storm alert until the next
        // weather cycle comes around.
        char sslbuf[96] = {0};
        int sslErr = client.lastError(sslbuf, sizeof(sslbuf));
        Log.println("NWS alerts fetch failed (" + String(code) + ") sslErr=" + String(sslErr) +
                    " " + sslbuf + " | heap free=" + String(ESP.getFreeHeap()) +
                    " maxAlloc=" + String(ESP.getMaxAllocHeap()) + " - retrying once");
        http.end();
        delay(1500);
        if (!http.begin(client, url)) return "";
        http.addHeader("User-Agent",
                       "ESP32WorldClock (github.com/echo-cool/cyd-world-clock)");
        http.addHeader("Accept", "application/geo+json");
        code = http.GET();
    }
    if (code != HTTP_CODE_OK)
    {
        if (code != HTTP_CODE_NOT_FOUND) // 404 = point outside NWS coverage; quiet
        {
            char sslbuf[96] = {0};
            int sslErr = client.lastError(sslbuf, sizeof(sslbuf));
            Log.println("NWS alerts fetch failed, HTTP " + String(code) + " sslErr=" +
                        String(sslErr) + " " + sslbuf + " | heap free=" +
                        String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));
        }
        http.end();
        return "";
    }
    Log.println("NWS alerts fetch OK, HTTP " + String(code) + " | heap free=" +
                String(ESP.getFreeHeap()) + " maxAlloc=" + String(ESP.getMaxAllocHeap()));
    StaticJsonDocument<128> filter;
    filter["features"][0]["properties"]["event"] = true;
    filter["features"][0]["properties"]["severity"] = true;

    DynamicJsonDocument doc(4096);
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err)
    {
        Log.println(String("NWS alerts parse failed: ") + err.c_str());
        return "";
    }
    else{
        Log.println("NWS alerts parse OK | heap free=" + String(ESP.getFreeHeap()) +
                    " maxAlloc=" + String(ESP.getMaxAllocHeap()));
    }

    const char *best = nullptr;
    int bestRank = -1;
    Log.println("NWS alerts: " + String(doc["features"].size()) + " active" + " At " + String(lat, 4) + "," + String(lon, 4));
    for (JsonObject f : doc["features"].as<JsonArray>())
    {
        const char *ev = f["properties"]["event"];
        if (!ev) continue;
        int rank = severityRank(f["properties"]["severity"]);
        if (rank > bestRank)
        {
            bestRank = rank;
            best = ev;
        }
    }
    return best ? normalizeAlertText(String(best)) : "";
}

// Extract one location's block of an Open-Meteo response into a ZoneWeather
// plus its precipitation-transition notice and the 15-minute precipitation
// series for the rain chart. Shared by the single-location (plain object)
// and multi-location (array element) response shapes.
static void parseLocationWeather(JsonVariant loc, ZoneWeather &w, String &notice,
                                 float *precipMm, uint8_t *precipCode,
                                 int &precipCount)
{
    JsonObject cur = loc["current"];
    w.tempC = cur["temperature_2m"].as<float>();
    w.weatherCode = cur["weather_code"].as<int>();
    w.humidity = cur["relative_humidity_2m"].isNull()
                     ? -1
                     : cur["relative_humidity_2m"].as<int>();
    w.valid = !cur.isNull();

    JsonObject daily = loc["daily"];
    w.hasMinMax = !daily["temperature_2m_max"][0].isNull() &&
                  !daily["temperature_2m_min"][0].isNull();
    if (w.hasMinMax)
    {
        w.tempMaxC = daily["temperature_2m_max"][0].as<float>();
        w.tempMinC = daily["temperature_2m_min"][0].as<float>();
    }

    JsonArray codes = loc["minutely_15"]["weather_code"].as<JsonArray>();
    notice = precipTransitionNotice(w.weatherCode, codes);

    // 15-minute precipitation amounts + codes for the rain chart. Both
    // arrays come from the same forecast, so their indices line up.
    JsonArray amounts = loc["minutely_15"]["precipitation"].as<JsonArray>();
    precipCount = 0;
    if (!amounts.isNull())
    {
        for (int k = 0; k < (int)amounts.size() && k < PRECIP_FORECAST_STEPS; k++)
        {
            precipMm[k] = amounts[k].as<float>();
            precipCode[k] = (uint8_t)(k < (int)codes.size() ? codes[k].as<int>() : 0);
            precipCount = k + 1;
        }
    }
}

// One fetch attempt, run on the task core. The blocking network work happens
// on a local copy of the coordinates; results are committed under the mutex
// at the end (and discarded if the zones changed mid-fetch).
static bool performFetch()
{
    WeatherLoc locs[4];
    weatherLock();
    memcpy(locs, weatherLocs, sizeof(locs));
    uint32_t generation = locsGeneration;
    weatherUnlock();

    // One multi-location request for every zone whose timezone maps to a
    // known preset city. Response order matches request order.
    String lats, lons;
    int zoneForSlot[4];
    int n = 0;
    for (int i = 0; i < 4; i++)
    {
        if (!locs[i].has) continue;
        if (n > 0)
        {
            lats += ",";
            lons += ",";
        }
        lats += String(locs[i].lat, 2);
        lons += String(locs[i].lon, 2);
        zoneForSlot[n++] = i;
    }
    if (n == 0) return false;

    // Plain HTTP, not HTTPS. This device can't reliably complete a TLS
    // handshake mid-run: mbedTLS's ~45KB context (two 16KB record buffers plus
    // the RSA/BIGNUM temporaries) can't be carved from the fragmented heap, so
    // the handshake dies with -1 ("RSA - public key operation failed : BIGNUM -
    // Memory allocation failed"). open-meteo serves this endpoint over http://
    // directly (no redirect to https), and it's public read-only data, so we
    // drop TLS entirely rather than fight the memory.
    ZoneWeather fresh[4] = {};
    String freshPrecipNotice[4];
    float freshPrecipMm[4][PRECIP_FORECAST_STEPS] = {};
    uint8_t freshPrecipCode[4][PRECIP_FORECAST_STEPS] = {};
    int freshPrecipCount[4] = {};
    {
        // timezone=auto makes the one-day daily high/low aggregate over each
        // location's own calendar day instead of the GMT day.
        String url = "http://api.open-meteo.com/v1/forecast?latitude=" + lats +
                     "&longitude=" + lons +
                     "&current=temperature_2m,relative_humidity_2m,weather_code"
                     "&daily=temperature_2m_max,temperature_2m_min"
                     "&forecast_days=1&timezone=auto"
                     "&minutely_15=weather_code,precipitation"
                     "&forecast_minutely_15=" + String(PRECIP_FORECAST_STEPS);
        Log.println("Fetching weather: " + url);

        WiFiClient client;
        HTTPClient http;
        http.setConnectTimeout(4000);
        http.setTimeout(6000);
        http.setReuse(false);
        if (!http.begin(client, url)) return false;
        // Force HTTP/1.0 so open-meteo returns a plain Connection: close body
        // rather than a chunked one (the chunked reader can hand back an empty body
        // that ArduinoJson rejects as EmptyInput).
        http.useHTTP10(true);
        int code = http.GET();
        if (code != HTTP_CODE_OK)
        {
            Log.println("Weather fetch failed, HTTP " + String(code));
            http.end();
            return false;
        }

        // A JSON filter keeps only each location's current temperature/weather
        // code plus the short 15-minute weather-code forecast, so the document
        // stays small no matter how much boilerplate open-meteo wraps around it.
        // Keep the document scoped to this block so its heap is released before
        // the later NWS HTTPS handshakes.
        StaticJsonDocument<512> filter;
        if (n == 1)
        {
            filter["current"]["temperature_2m"] = true;
            filter["current"]["relative_humidity_2m"] = true;
            filter["current"]["weather_code"] = true;
            filter["daily"]["temperature_2m_max"] = true;
            filter["daily"]["temperature_2m_min"] = true;
            filter["minutely_15"]["weather_code"] = true;
            filter["minutely_15"]["precipitation"] = true;
        }
        else
        {
            filter[0]["current"]["temperature_2m"] = true;
            filter[0]["current"]["relative_humidity_2m"] = true;
            filter[0]["current"]["weather_code"] = true;
            filter[0]["daily"]["temperature_2m_max"] = true;
            filter[0]["daily"]["temperature_2m_min"] = true;
            filter[0]["minutely_15"]["weather_code"] = true;
            filter[0]["minutely_15"]["precipitation"] = true;
        }

        DynamicJsonDocument doc(8192);
        DeserializationError err =
            deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        http.end();
        if (err)
        {
            Log.println(String("Weather JSON parse failed: ") + err.c_str());
            return false;
        }

        // Multi-location responses are a JSON array; a single location comes back
        // as a plain object.
        if (doc.is<JsonArray>())
        {
            JsonArray arr = doc.as<JsonArray>();
            for (int k = 0; k < n && k < (int)arr.size(); k++)
            {
                int zi = zoneForSlot[k];
                parseLocationWeather(arr[k], fresh[zi], freshPrecipNotice[zi],
                                     freshPrecipMm[zi], freshPrecipCode[zi],
                                     freshPrecipCount[zi]);
            }
        }
        else
        {
            int zi = zoneForSlot[0];
            parseLocationWeather(doc.as<JsonVariant>(), fresh[zi],
                                 freshPrecipNotice[zi], freshPrecipMm[zi],
                                 freshPrecipCode[zi], freshPrecipCount[zi]);
        }
    }

    // Weather alerts (when enabled): US cities pull the NWS active-alerts feed;
    // others derive a severe-condition alert from the code just fetched. Done
    // outside the lock (NWS calls are slow) and committed with the weather.
    String freshAlert[4];
    bool freshAlertOfficial[4] = {};
    if (projectConfig.weatherAlerts)
    {
        for (int k = 0; k < n; k++)
        {
            int zi = zoneForSlot[k];
            if (locs[zi].isUS)
            {
                freshAlert[zi] = fetchUsAlert(locs[zi].lat, locs[zi].lon);
                freshAlertOfficial[zi] = freshAlert[zi].length() > 0;
            }
            else if (fresh[zi].valid)
            {
                freshAlert[zi] = String(severeWeatherText(fresh[zi].weatherCode));
            }
        }
    }

    weatherLock();
    if (generation != locsGeneration)
    {
        // A zone changed while this request was in flight - the data belongs
        // to the old cities. Drop it; the forced refetch is already queued.
        weatherUnlock();
        return false;
    }
    for (int i = 0; i < 4; i++)
    {
        zoneWeather[i] = fresh[i];
        zoneAlert[i] = freshAlert[i];
        zoneAlertOfficial[i] = freshAlertOfficial[i];
        zonePrecipNotice[i] = freshPrecipNotice[i];
        memcpy(zonePrecipMm[i], freshPrecipMm[i], sizeof(zonePrecipMm[i]));
        memcpy(zonePrecipCode[i], freshPrecipCode[i], sizeof(zonePrecipCode[i]));
        zonePrecipCount[i] = freshPrecipCount[i];
    }
    dataVersion++;
    weatherUnlock();

    Log.println("Weather updated for " + String(n) + " zone(s)");
    return true;
}

static void weatherTask(void *)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(WEATHER_TASK_TICK_MS));

        // The holiday-calendar refreshes share this task (and its HTTPS-sized
        // stack) instead of paying for further 16KB tasks of their own.
        marketHolidaysTick();
        holidaysTick();

        // Distinguish "really online" from "associated but behind a captive
        // portal" here on core 0 (self-rate-limited to ~60s) so the blocking
        // probe never stalls the render loop on core 1. Skipped while the login
        // relay is up, since it runs its own probe on the main core.
        if (!otaInProgress && !wifiRelayActive()) netCheckService();

        if (otaInProgress) continue;                 // don't fetch mid-update
        if (WiFi.status() != WL_CONNECTED) continue; // recheck next tick

        unsigned long now = millis();
        weatherLock();
        bool due = fetchForced || !attempted ||
                   (now - lastAttemptMillis >= (succeeded ? weatherRefreshMs() : WEATHER_RETRY_MS));
        if (due)
        {
            fetchForced = false;
            attempted = true;
            lastAttemptMillis = now;
        }
        weatherUnlock();
        if (!due) continue;

        bool ok = performFetch();

        weatherLock();
        succeeded = ok;
        if (ok) lastSuccessMillis = millis();
        weatherUnlock();
    }
}

void weatherBegin()
{
    if (weatherMutex) return; // already running
    weatherMutex = xSemaphoreCreateMutex();
    weatherLock();
    snapshotLocationsLocked();
    weatherUnlock();

    // Core 0 (the Arduino loop runs on core 1). 16KB stack: HTTPS through
    // WiFiClientSecure needs far more headroom than the FreeRTOS default.
    xTaskCreatePinnedToCore(weatherTask, "weather", 16384, nullptr, 1, nullptr, 0);
}

int displayTemp(float tempC)
{
    float t = projectConfig.useFahrenheit ? tempC * 9.0f / 5.0f + 32.0f : tempC;
    return (int)lroundf(t);
}

char tempUnitLetter()
{
    return projectConfig.useFahrenheit ? 'F' : 'C';
}

// WMO weather interpretation codes:
// https://open-meteo.com/en/docs#weather_variable_documentation
const char *weatherCodeText(int code)
{
    switch (code) {
    case 0: return "CLEAR";
    case 1: return "MOSTLY CLEAR";
    case 2: return "PARTLY CLOUDY";
    case 3: return "OVERCAST";
    case 45:
    case 48: return "FOG";
    case 51:
    case 53:
    case 55:
    case 56:
    case 57: return "DRIZZLE";
    case 61:
    case 63:
    case 65:
    case 66:
    case 67: return "RAIN";
    case 71:
    case 73:
    case 75:
    case 77: return "SNOW";
    case 80:
    case 81:
    case 82: return "SHOWERS";
    case 85:
    case 86: return "SNOW SHOWERS";
    case 95:
    case 96:
    case 99: return "THUNDERSTORM";
    default: return "UNKNOWN";
    }
}

uint16_t weatherCodeColor(int code)
{
    if (code == 0 || code == 1) return TFT_YELLOW;            // clear
    if (code == 2 || code == 3) return TFT_LIGHTGREY;         // clouds
    if (code == 45 || code == 48) return TFT_DARKGREY;        // fog
    if (code >= 51 && code <= 67) return TFT_CYAN;            // drizzle / rain
    if (code >= 71 && code <= 77) return TFT_WHITE;           // snow
    if (code >= 80 && code <= 82) return TFT_CYAN;            // showers
    if (code == 85 || code == 86) return TFT_WHITE;           // snow showers
    if (code >= 95) return TFT_ORANGE;                        // thunderstorm
    return TFT_WHITE;
}
