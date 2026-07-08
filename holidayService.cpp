#include "holidayService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ezTime.h>

#include "ClockLogic.h" // worldZones
#include "otaUpdate.h"  // otaInProgress - pause fetching during an update
#include "uiPages.h"    // getCountryForTimezone

static const int MAX_PUBLIC_HOLIDAYS = 32;

static const unsigned long HOLIDAYS_REFRESH_MS = 7UL * 24UL * 3600UL * 1000UL; // weekly
static const unsigned long HOLIDAYS_FAIL_BACKOFF_MS = 10UL * 60UL * 1000UL;    // after a failure

struct RenderBufferReleaseGuard
{
    bool released;
    RenderBufferReleaseGuard() : released(clockReleaseRenderBufferForNetwork()) {}
    ~RenderBufferReleaseGuard() { clockRestoreRenderBufferForNetwork(released); }
};

// Per-zone state. "want" is what the zone currently needs (country from the
// preset, year from the zone's local clock); "have" is what the last good
// fetch stored. The core-0 tick works to make have match want. All access
// is guarded by holMutex: the main core reads (render/queries) and writes
// the wants (service/invalidate); core 0 writes the fetched data.
struct ZoneHolidayState
{
    char country[3];  // wanted ISO code; "" = no data available for this zone
    int wantYear;     // zone's current local year; 0 until the clock is set
    char haveCountry[3];
    int haveYear;
    bool valid;
    bool stale; // weekly refresh due - old data stays visible until replaced
    unsigned long fetchedMs;
    uint16_t count;
    PublicHoliday items[MAX_PUBLIC_HOLIDAYS];
};

static SemaphoreHandle_t holMutex = nullptr;
static ZoneHolidayState zoneHol[4];
static uint32_t holDataVersion = 0;
static unsigned long lastFailMs = 0; // 0 = last attempt succeeded

static void holLock()
{
    if (holMutex) xSemaphoreTake(holMutex, portMAX_DELAY);
}

static void holUnlock()
{
    if (holMutex) xSemaphoreGive(holMutex);
}

// Refresh each zone's wanted country from the configured timezones. MAIN
// core only (reads the worldZones Strings); callers hold the mutex.
static void snapshotCountriesLocked()
{
    for (int i = 0; i < 4; i++)
    {
        const char *cc = getCountryForTimezone(worldZones[i].timezone);
        strlcpy(zoneHol[i].country, cc ? cc : "", sizeof(zoneHol[i].country));
        // A zone that moved to a different country invalidates immediately
        // (a same-country move, e.g. Denver -> Chicago, keeps its data).
        if (zoneHol[i].valid && strcmp(zoneHol[i].country, zoneHol[i].haveCountry) != 0)
        {
            zoneHol[i].valid = false;
            zoneHol[i].count = 0;
            holDataVersion++;
        }
    }
}

void holidaysBegin()
{
    if (holMutex) return; // already initialized
    holMutex = xSemaphoreCreateMutex();
    holLock();
    snapshotCountriesLocked();
    holUnlock();
}

void holidaysInvalidate()
{
    holLock();
    snapshotCountriesLocked();
    holUnlock();
}

void holidaysService()
{
    static unsigned long lastCheck = 0;
    unsigned long ms = millis();
    if (lastCheck != 0 && ms - lastCheck < 60000UL)
        return;
    lastCheck = ms;

    for (int i = 0; i < 4; i++)
    {
        if (!worldZones[i].initialized) continue;
        time_t local = worldZones[i].tz.now();
        if (local < 1000000000) continue; // clock not set yet
        int y = year(local);

        holLock();
        zoneHol[i].wantYear = y; // year rollover simply changes the want
        if (zoneHol[i].valid && ms - zoneHol[i].fetchedMs > HOLIDAYS_REFRESH_MS)
        {
            zoneHol[i].stale = true;
        }
        holUnlock();
    }
}

bool getHolidayName(int zoneIdx, uint32_t ymd, char *out, size_t outLen)
{
    if (zoneIdx < 0 || zoneIdx > 3) return false;
    bool found = false;
    holLock();
    if (zoneHol[zoneIdx].valid)
    {
        for (int i = 0; i < zoneHol[zoneIdx].count; i++)
        {
            if (zoneHol[zoneIdx].items[i].date == ymd)
            {
                if (out && outLen > 0)
                {
                    strlcpy(out, zoneHol[zoneIdx].items[i].name, outLen);
                }
                found = true;
                break;
            }
        }
    }
    holUnlock();
    return found;
}

int getZoneHolidays(int zoneIdx, PublicHoliday *out, int maxOut)
{
    if (zoneIdx < 0 || zoneIdx > 3 || !out || maxOut <= 0) return 0;
    holLock();
    int n = zoneHol[zoneIdx].valid ? zoneHol[zoneIdx].count : 0;
    if (n > maxOut) n = maxOut;
    memcpy(out, zoneHol[zoneIdx].items, n * sizeof(PublicHoliday));
    holUnlock();
    return n;
}

uint32_t holidaysDataVersion()
{
    holLock();
    uint32_t v = holDataVersion;
    holUnlock();
    return v;
}

int holidayZonesLoaded(int &eligible)
{
    int loaded = 0;
    eligible = 0;
    holLock();
    for (int i = 0; i < 4; i++)
    {
        if (zoneHol[i].country[0] == '\0') continue;
        eligible++;
        if (zoneHol[i].valid) loaded++;
    }
    holUnlock();
    return loaded;
}

// One country-year fetch. Runs on the core-0 task; results land in `out`.
// Keeps only nationwide public holidays (global == true, type "Public") so
// regional observances don't clutter the display.
static int fetchCountryYear(const char *country, int year, PublicHoliday *out, int maxOut)
{
    RenderBufferReleaseGuard renderMemory;

    String url = "https://date.nager.at/api/v3/PublicHolidays/" +
                 String(year) + "/" + country;
    Log.println("Fetching public holidays: " + url);

    WiFiClientSecure client;
    client.setInsecure(); // public calendar data - certificate pinning not worth the upkeep
    HTTPClient http;
    // date.nager.at is Azure-hosted and presents a long RSA certificate chain -
    // the same slow-handshake profile that makes api.weather.gov abort inside a
    // 4s connect window (-1) whenever the CPU is busy (see fetchUsAlert). Give
    // the handshake room, and retry once on a connection-layer failure.
    http.setConnectTimeout(12000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) return -1;
    int code = http.GET();
    if (code < 0)
    {
        char sslbuf[96] = {0};
        int sslErr = client.lastError(sslbuf, sizeof(sslbuf));
        Log.println("Holiday fetch failed (" + String(code) + ") sslErr=" + String(sslErr) +
                    " " + sslbuf + " | heap free=" + String(ESP.getFreeHeap()) +
                    " maxAlloc=" + String(ESP.getMaxAllocHeap()) + " - retrying once");
        http.end();
        delay(1500);
        if (!http.begin(client, url)) return -1;
        code = http.GET();
    }
    if (code != HTTP_CODE_OK)
    {
        char sslbuf[96] = {0};
        int sslErr = client.lastError(sslbuf, sizeof(sslbuf));
        Log.println("Holiday fetch failed, HTTP " + String(code) + " sslErr=" + String(sslErr) +
                    " " + sslbuf + " | heap free=" + String(ESP.getFreeHeap()) +
                    " maxAlloc=" + String(ESP.getMaxAllocHeap()));
        http.end();
        return -1;
    }
    String payload = http.getString();
    http.end();

    // Parse with a filter that keeps only the fields read below. The
    // unfiltered US payload carries per-state "counties" arrays large enough
    // to overflow any sane document budget - the old unfiltered parse died
    // with NoMemory every time, so US holidays never loaded at all.
    StaticJsonDocument<256> filter;
    JsonObject filterEntry = filter.createNestedObject();
    filterEntry["date"] = true;
    filterEntry["name"] = true;
    filterEntry["global"] = true;
    filterEntry["types"] = true;

    DynamicJsonDocument doc(16384);
    DeserializationError err =
        deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (err)
    {
        Log.println(String("Public holiday JSON parse failed: ") + err.c_str());
        return -1;
    }
    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull()) return -1;

    int n = 0;
    for (JsonObject h : arr)
    {
        if (n >= maxOut) break;
        if (!h["global"].as<bool>()) continue; // skip regional holidays

        JsonArray types = h["types"];
        bool isPublic = types.isNull(); // older API versions omit "types"
        for (JsonVariant t : types)
        {
            const char *ts = t.as<const char *>();
            if (ts && strcmp(ts, "Public") == 0)
            {
                isPublic = true;
                break;
            }
        }
        if (!isPublic) continue;

        const char *ds = h["date"];   // "YYYY-MM-DD"
        const char *name = h["name"]; // English name (display font is ASCII)
        if (!ds || strlen(ds) < 10 || !name) continue;
        uint32_t y = (uint32_t)atoi(ds);
        uint32_t mo = (uint32_t)atoi(ds + 5);
        uint32_t dd = (uint32_t)atoi(ds + 8);
        if (mo < 1 || mo > 12 || dd < 1 || dd > 31) continue;

        out[n].date = y * 10000u + mo * 100u + dd;
        strlcpy(out[n].name, name, sizeof(out[n].name));
        for (char *p = out[n].name; *p; p++) *p = toupper((unsigned char)*p);
        n++;
    }
    return n;
}

void holidaysTick()
{
    if (otaInProgress || WiFi.status() != WL_CONNECTED) return;
    if (lastFailMs != 0 && millis() - lastFailMs < HOLIDAYS_FAIL_BACKOFF_MS) return;

    // Pick one zone whose data is missing, wrong or stale.
    char country[3] = "";
    int year = 0;
    holLock();
    for (int i = 0; i < 4; i++)
    {
        ZoneHolidayState &z = zoneHol[i];
        if (z.country[0] == '\0' || z.wantYear < 2020) continue;
        if (z.valid && !z.stale && z.haveYear == z.wantYear &&
            strcmp(z.haveCountry, z.country) == 0) continue;
        strlcpy(country, z.country, sizeof(country));
        year = z.wantYear;
        break;
    }
    holUnlock();
    if (country[0] == '\0') return; // everything up to date

    static PublicHoliday fetched[MAX_PUBLIC_HOLIDAYS]; // too big for the stack
    int n = fetchCountryYear(country, year, fetched, MAX_PUBLIC_HOLIDAYS);
    if (n < 0)
    {
        lastFailMs = millis();
        return;
    }
    lastFailMs = 0;

    // Store into every zone that wants this same country-year.
    holLock();
    for (int i = 0; i < 4; i++)
    {
        ZoneHolidayState &z = zoneHol[i];
        if (strcmp(z.country, country) != 0 || z.wantYear != year) continue;
        memcpy(z.items, fetched, n * sizeof(PublicHoliday));
        z.count = (uint16_t)n;
        strlcpy(z.haveCountry, country, sizeof(z.haveCountry));
        z.haveYear = year;
        z.valid = true;
        z.stale = false;
        z.fetchedMs = millis();
    }
    holDataVersion++;
    holUnlock();
    Log.println("Public holidays stored: " + String(country) + " " +
                   String(year) + " (" + String(n) + " entries)");
}

void printPublicHolidaysStatus()
{
    Log.println("=== Public holidays (date.nager.at) ===");
    holLock();
    for (int i = 0; i < 4; i++)
    {
        ZoneHolidayState &z = zoneHol[i];
        String line = "Zone " + String(i) + " (" + worldZones[i].name + "): ";
        if (z.country[0] == '\0')
        {
            line += "no country data for this timezone";
        }
        else if (!z.valid)
        {
            line += String(z.country) + " - not fetched yet";
        }
        else
        {
            line += String(z.haveCountry) + " " + String(z.haveYear) + ", " +
                    String(z.count) + " holidays" + (z.stale ? " (refresh due)" : "");
        }
        Log.println(line);
    }
    holUnlock();
}
