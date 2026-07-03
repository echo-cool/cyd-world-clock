#include "weatherService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "ClockLogic.h"     // worldZones
#include "holidayService.h" // holidaysTick - shares this task's HTTPS stack
#include "marketHolidays.h" // marketHolidaysTick - shares this task's HTTPS stack
#include "otaUpdate.h"      // otaInProgress - pause fetching during an update
#include "uiPages.h"        // getCityCoords

const unsigned long WEATHER_REFRESH_MS = 20UL * 60UL * 1000UL; // after a success
const unsigned long WEATHER_RETRY_MS = 5UL * 60UL * 1000UL;    // after a failure
const unsigned long WEATHER_TASK_TICK_MS = 2000;               // due-check cadence

// City coordinates the fetch task works from. Snapshotted from worldZones on
// the MAIN core (weatherBegin / weatherInvalidate), so the task never reads
// the zones' Strings while the touch UI might be reassigning them.
struct WeatherLoc
{
    bool has;
    float lat;
    float lon;
};

// Everything below is shared between the main core (readers, invalidate) and
// the fetch task on core 0 (writer); guard every access with weatherMutex.
static SemaphoreHandle_t weatherMutex = nullptr;
static WeatherLoc weatherLocs[4] = {};
static uint32_t locsGeneration = 0; // bumped on snapshot; stale fetches discard
static ZoneWeather zoneWeather[4] = {};
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
    }
    locsGeneration++;
}

ZoneWeather getZoneWeather(int i)
{
    if (i < 0 || i > 3) return {false, 0, 0};
    weatherLock();
    ZoneWeather w = zoneWeather[i];
    weatherUnlock();
    return w;
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
    for (int i = 0; i < 4; i++) zoneWeather[i].valid = false;
    attempted = false;
    succeeded = false;
    fetchForced = true;
    dataVersion++; // the face repaints its rows as "--" right away
    weatherUnlock();
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

    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + lats +
                 "&longitude=" + lons + "&current=temperature_2m,weather_code";
    Log.println("Fetching weather: " + url);

    WiFiClientSecure client;
    client.setInsecure(); // public weather data - certificate pinning not worth the upkeep
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        Log.println("Weather fetch failed, HTTP " + String(code));
        http.end();
        return false;
    }
    String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
    {
        Log.println(String("Weather JSON parse failed: ") + err.c_str());
        return false;
    }

    ZoneWeather fresh[4] = {};

    // Multi-location responses are a JSON array; a single location comes back
    // as a plain object.
    if (doc.is<JsonArray>())
    {
        JsonArray arr = doc.as<JsonArray>();
        for (int k = 0; k < n && k < (int)arr.size(); k++)
        {
            JsonObject cur = arr[k]["current"];
            int zi = zoneForSlot[k];
            fresh[zi].tempC = cur["temperature_2m"].as<float>();
            fresh[zi].weatherCode = cur["weather_code"].as<int>();
            fresh[zi].valid = !cur.isNull();
        }
    }
    else
    {
        JsonObject cur = doc["current"];
        int zi = zoneForSlot[0];
        fresh[zi].tempC = cur["temperature_2m"].as<float>();
        fresh[zi].weatherCode = cur["weather_code"].as<int>();
        fresh[zi].valid = !cur.isNull();
    }

    weatherLock();
    if (generation != locsGeneration)
    {
        // A zone changed while this request was in flight - the data belongs
        // to the old cities. Drop it; the forced refetch is already queued.
        weatherUnlock();
        return false;
    }
    for (int i = 0; i < 4; i++) zoneWeather[i] = fresh[i];
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

        if (otaInProgress) continue;                 // don't fetch mid-update
        if (WiFi.status() != WL_CONNECTED) continue; // recheck next tick

        unsigned long now = millis();
        weatherLock();
        bool due = fetchForced || !attempted ||
                   (now - lastAttemptMillis >= (succeeded ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS));
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
