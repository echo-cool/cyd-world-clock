#include "marketHolidays.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ezTime.h>

#include "ClockLogic.h" // release 38KB quadrant sprite around HTTPS handshakes
#include "logBuffer.h" // Log
#include "otaUpdate.h" // otaInProgress - don't fetch mid-update

// MARKET_HOLIDAYS_URL can be overridden in the untracked secrets.h.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef MARKET_HOLIDAYS_URL
#define MARKET_HOLIDAYS_URL \
    "https://raw.githubusercontent.com/echo-cool/cyd-world-clock/main/marketHolidays.json"
#endif

// ---------------------------------------------------------------------------
// Compiled-in closure calendars for the exchanges the clock knows about.
// These are the fallback layer; the fetched marketHolidays.json (see the
// bottom of this file) overrides them per exchange when available.
//
// Dates are encoded as YYYYMMDD integers; only weekday closures are listed
// (weekends are already handled by the market-status logic). Half-day early
// closes (NYSE Black Friday / Christmas Eve 1pm closes, HKEX Lunar New Year
// eve half days, ...) live in the separate EARLY_CLOSE_TABLES below - the
// market status truncates the trading sessions at the early-close time.
//
// Maintenance: NYSE publishes its calendar ~3 years ahead; LSE follows the
// England & Wales bank holidays; the SSE / TSE / HKEX schedules are announced
// annually (China's State Council typically in Nov/Dec, the HK government in
// May, JPX per Japan's national-holiday law). Dates past a table's horizon
// simply aren't holiday-aware - extend the table when the next year's
// schedule is published.
//
// Sources (verified July 2026):
//   NYSE: https://www.nyse.com/trade/hours-calendars
//   LSE:  https://www.londonstockexchange.com/equities-trading/business-days
//         (England & Wales bank holidays: https://www.gov.uk/bank-holidays)
//   SSE:  State Council 2026 holiday schedule (mirrored by SHFE/CFFEX notices)
//   TSE:  https://www.jpx.co.jp/english/corporate/about-jpx/calendar/
//   HKEX: https://www.gov.hk/en/about/abouthk/holiday/2026.htm
// ---------------------------------------------------------------------------

static const uint32_t NYSE_HOLIDAYS[] = {
    // 2026
    20260101, // New Year's Day
    20260119, // Martin Luther King Jr. Day
    20260216, // Washington's Birthday
    20260403, // Good Friday
    20260525, // Memorial Day
    20260619, // Juneteenth
    20260703, // Independence Day (observed - Jul 4 is a Saturday)
    20260907, // Labor Day
    20261126, // Thanksgiving
    20261225, // Christmas
    // 2027
    20270101, // New Year's Day
    20270118, // Martin Luther King Jr. Day
    20270215, // Washington's Birthday
    20270326, // Good Friday
    20270531, // Memorial Day
    20270618, // Juneteenth (observed - Jun 19 is a Saturday)
    20270705, // Independence Day (observed - Jul 4 is a Sunday)
    20270906, // Labor Day
    20271125, // Thanksgiving
    20271224, // Christmas (observed - Dec 25 is a Saturday)
};

static const uint32_t LSE_HOLIDAYS[] = {
    // 2026 (England & Wales bank holidays)
    20260101, // New Year's Day
    20260403, // Good Friday
    20260406, // Easter Monday
    20260504, // Early May bank holiday
    20260525, // Spring bank holiday
    20260831, // Summer bank holiday
    20261225, // Christmas Day
    20261228, // Boxing Day (substitute - Dec 26 is a Saturday)
    // 2027
    20270101, // New Year's Day
    20270326, // Good Friday
    20270329, // Easter Monday
    20270503, // Early May bank holiday
    20270531, // Spring bank holiday
    20270830, // Summer bank holiday
    20271227, // Christmas Day (substitute - Dec 25 is a Saturday)
    20271228, // Boxing Day (substitute - Dec 26 is a Sunday)
};

static const uint32_t SSE_HOLIDAYS[] = {
    // 2026 only - the State Council announces each year's schedule in Nov/Dec
    20260101, 20260102,           // New Year
    20260216, 20260217, 20260218, // Spring Festival
    20260219, 20260220, 20260223, //   (closed Feb 16-23; weekdays listed)
    20260406,                     // Qingming Festival (observed)
    20260501, 20260504, 20260505, // Labour Day (closed May 1-5; weekdays)
    20260619,                     // Dragon Boat Festival
    20260925,                     // Mid-Autumn Festival
    20261001, 20261002, 20261005, // National Day golden week
    20261006, 20261007,           //   (closed Oct 1-7; weekdays listed)
};

static const uint32_t TSE_HOLIDAYS[] = {
    // 2026 only - future years depend on Japan's national-holiday law
    20260101, 20260102,           // New Year (exchange closed Jan 1-3)
    20260112,                     // Coming of Age Day
    20260211,                     // National Foundation Day
    20260223,                     // Emperor's Birthday
    20260320,                     // Vernal Equinox Day
    20260429,                     // Showa Day
    20260504, 20260505, 20260506, // Golden Week (Greenery / Children's / Constitution obs.)
    20260720,                     // Marine Day
    20260811,                     // Mountain Day
    20260921, 20260922, 20260923, // Silver Week (Respect for the Aged / bridge / Equinox)
    20261012,                     // Sports Day
    20261103,                     // Culture Day
    20261123,                     // Labor Thanksgiving Day
    20261231,                     // Year-end exchange closure
};

static const uint32_t HKEX_HOLIDAYS[] = {
    // 2026 only - the HK government publishes each year's list in May
    20260101,                     // New Year's Day
    20260217, 20260218, 20260219, // Lunar New Year (days 1-3)
    20260403,                     // Good Friday
    20260406,                     // Easter Monday
    20260407,                     // Ching Ming Festival (observed)
    20260501,                     // Labour Day
    20260525,                     // Buddha's Birthday (observed)
    20260619,                     // Tuen Ng (Dragon Boat) Festival
    20260701,                     // HKSAR Establishment Day
    20261001,                     // National Day
    20261019,                     // Chung Yeung Festival (observed)
    20261225,                     // Christmas Day
};

struct HolidayTable
{
    const char *exchange;
    const uint32_t *dates;
    size_t count;
};

static const HolidayTable HOLIDAY_TABLES[] = {
    {"NYSE", NYSE_HOLIDAYS, sizeof(NYSE_HOLIDAYS) / sizeof(NYSE_HOLIDAYS[0])},
    {"LSE", LSE_HOLIDAYS, sizeof(LSE_HOLIDAYS) / sizeof(LSE_HOLIDAYS[0])},
    {"SSE", SSE_HOLIDAYS, sizeof(SSE_HOLIDAYS) / sizeof(SSE_HOLIDAYS[0])},
    {"TSE", TSE_HOLIDAYS, sizeof(TSE_HOLIDAYS) / sizeof(TSE_HOLIDAYS[0])},
    {"HKEX", HKEX_HOLIDAYS, sizeof(HKEX_HOLIDAYS) / sizeof(HKEX_HOLIDAYS[0])},
};

// ---------------------------------------------------------------------------
// Compiled-in half-day early closes: the exchange trades normally until
// closeMinutes (minutes since local midnight), then closes for the day.
// SSE and TSE have no scheduled half days.
//
// NYSE: 1:00 PM on the day after Thanksgiving and (when it's a weekday)
// Christmas Eve. LSE: 12:30 on Christmas Eve and New Year's Eve. HKEX: noon
// on Lunar New Year's Eve, Christmas Eve and New Year's Eve (the morning
// session simply has no afternoon counterpart).
// ---------------------------------------------------------------------------

struct EarlyClose
{
    uint32_t date;         // YYYYMMDD, exchange-local
    uint16_t closeMinutes; // minutes since local midnight
};

static const EarlyClose NYSE_EARLY_CLOSES[] = {
    {20261127, 13 * 60}, // day after Thanksgiving, 1:00 PM
    {20261224, 13 * 60}, // Christmas Eve, 1:00 PM
    {20271126, 13 * 60}, // day after Thanksgiving, 1:00 PM
};

static const EarlyClose LSE_EARLY_CLOSES[] = {
    {20261224, 12 * 60 + 30}, // Christmas Eve, 12:30
    {20261231, 12 * 60 + 30}, // New Year's Eve, 12:30
    {20271224, 12 * 60 + 30}, // Christmas Eve, 12:30
    {20271231, 12 * 60 + 30}, // New Year's Eve, 12:30
};

static const EarlyClose HKEX_EARLY_CLOSES[] = {
    {20260216, 12 * 60}, // Lunar New Year's Eve, noon
    {20261224, 12 * 60}, // Christmas Eve, noon
    {20261231, 12 * 60}, // New Year's Eve, noon
};

struct EarlyCloseTable
{
    const char *exchange;
    const EarlyClose *entries;
    size_t count;
};

static const EarlyCloseTable EARLY_CLOSE_TABLES[] = {
    {"NYSE", NYSE_EARLY_CLOSES, sizeof(NYSE_EARLY_CLOSES) / sizeof(NYSE_EARLY_CLOSES[0])},
    {"LSE", LSE_EARLY_CLOSES, sizeof(LSE_EARLY_CLOSES) / sizeof(LSE_EARLY_CLOSES[0])},
    {"HKEX", HKEX_EARLY_CLOSES, sizeof(HKEX_EARLY_CLOSES) / sizeof(HKEX_EARLY_CLOSES[0])},
};

// ---------------------------------------------------------------------------
// Fetched override tables.
//
// A weekly HTTPS fetch of MARKET_HOLIDAYS_URL keeps the calendars current
// without reflashing. The payload is cached in SPIFFS (first line = fetch
// time as a unix timestamp, remainder = the JSON payload) so a reboot starts
// from the last good data. All shared state below is guarded by
// holidayMutex: readers are on the main core (isMarketHoliday, the service /
// serial functions), the writer is the core-0 fetch task.
// ---------------------------------------------------------------------------

#define HOLIDAY_CACHE_FILE "/market_holidays.json"

static const time_t HOLIDAY_REFRESH_SECONDS = 7 * 24 * 3600; // weekly
static const unsigned long HOLIDAY_RETRY_MS = 6UL * 3600UL * 1000UL; // after a failure

static const int MAX_DYN_EXCHANGES = 8;
static const int MAX_DYN_DATES = 88;
static const int MAX_DYN_EARLY = 12;

struct RenderBufferReleaseGuard
{
    bool released;
    RenderBufferReleaseGuard() : released(clockReleaseRenderBufferForNetwork()) {}
    ~RenderBufferReleaseGuard() { clockRestoreRenderBufferForNetwork(released); }
};

struct DynHolidayTable
{
    char exchange[8];
    uint16_t count;
    uint32_t dates[MAX_DYN_DATES];
    // Half-day early closes ("earlyCloses" in the fetched JSON)
    uint16_t earlyCount;
    uint32_t earlyDates[MAX_DYN_EARLY];
    uint16_t earlyCloseMin[MAX_DYN_EARLY];
};

static SemaphoreHandle_t holidayMutex = nullptr;
static DynHolidayTable dynTables[MAX_DYN_EXCHANGES];
static int dynTableCount = 0;
static time_t lastFetchUnix = 0;         // unix time of the last good fetch
static unsigned long lastAttemptMs = 0;  // millis() of the last attempt
static bool fetchPending = false;        // set by the main core, consumed on core 0
static time_t scheduledNowUtc = 0;       // "current" unix time snapshot for the fetcher

static void holidayLock()
{
    if (holidayMutex) xSemaphoreTake(holidayMutex, portMAX_DELAY);
}

static void holidayUnlock()
{
    if (holidayMutex) xSemaphoreGive(holidayMutex);
}

bool isMarketHoliday(const String &exchange, int y, int m, int d)
{
    const uint32_t key = (uint32_t)y * 10000u + (uint32_t)m * 100u + (uint32_t)d;

    // Fetched data first: an exchange present there overrides its compiled
    // table entirely (including "not a holiday" answers).
    holidayLock();
    for (int t = 0; t < dynTableCount; t++)
    {
        if (exchange != dynTables[t].exchange)
            continue;
        for (int i = 0; i < dynTables[t].count; i++)
        {
            if (dynTables[t].dates[i] == key)
            {
                holidayUnlock();
                return true;
            }
        }
        holidayUnlock();
        return false;
    }
    holidayUnlock();

    for (const HolidayTable &t : HOLIDAY_TABLES)
    {
        if (exchange != t.exchange)
            continue;
        for (size_t i = 0; i < t.count; i++)
        {
            if (t.dates[i] == key)
                return true;
        }
        return false;
    }
    return false;
}

int marketEarlyCloseMinutes(const String &exchange, int y, int m, int d)
{
    const uint32_t key = (uint32_t)y * 10000u + (uint32_t)m * 100u + (uint32_t)d;

    // An exchange present in the fetched data overrides its compiled early
    // closes entirely, same as the full-day holiday semantics above.
    holidayLock();
    for (int t = 0; t < dynTableCount; t++)
    {
        if (exchange != dynTables[t].exchange)
            continue;
        for (int i = 0; i < dynTables[t].earlyCount; i++)
        {
            if (dynTables[t].earlyDates[i] == key)
            {
                int minutes = dynTables[t].earlyCloseMin[i];
                holidayUnlock();
                return minutes;
            }
        }
        holidayUnlock();
        return -1;
    }
    holidayUnlock();

    for (const EarlyCloseTable &t : EARLY_CLOSE_TABLES)
    {
        if (exchange != t.exchange)
            continue;
        for (size_t i = 0; i < t.count; i++)
        {
            if (t.entries[i].date == key)
                return t.entries[i].closeMinutes;
        }
        return -1;
    }
    return -1;
}

// Parse a holidays JSON payload and, if it is sane, swap it in as the active
// override tables. Returns false (leaving the current tables untouched) on
// any parse or validation failure.
static bool applyHolidayJson(const String &payload)
{
    DynamicJsonDocument doc(16384);
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
    {
        Log.println(String("Holiday JSON parse failed: ") + err.c_str());
        return false;
    }

    JsonObject holidays = doc["holidays"];
    if (holidays.isNull())
    {
        Log.println("Holiday JSON rejected: no \"holidays\" object");
        return false;
    }

    // Optional half-day early closes, keyed by the same exchange names. Only
    // honored for exchanges that also appear in "holidays" (an exchange's
    // fetched entry replaces its compiled tables as a unit).
    JsonObject earlies = doc["earlyCloses"];

    // Build into a local staging copy first so a bad payload can't leave the
    // active tables half-updated.
    static DynHolidayTable staged[MAX_DYN_EXCHANGES]; // static: too big for the stack
    int stagedCount = 0;

    for (JsonPair kv : holidays)
    {
        if (stagedCount >= MAX_DYN_EXCHANGES)
            break;
        const char *exchange = kv.key().c_str();
        JsonArray dates = kv.value().as<JsonArray>();
        if (dates.isNull() || strlen(exchange) == 0 ||
            strlen(exchange) >= sizeof(staged[0].exchange))
            continue;

        DynHolidayTable &t = staged[stagedCount];
        strncpy(t.exchange, exchange, sizeof(t.exchange));
        t.exchange[sizeof(t.exchange) - 1] = '\0';
        t.count = 0;
        for (JsonVariant v : dates)
        {
            if (t.count >= MAX_DYN_DATES)
                break;
            uint32_t date = v.as<uint32_t>();
            // Plausibility: YYYYMMDD between 2020 and 2099
            if (date < 20200101u || date > 20991231u)
                continue;
            uint32_t mm = (date / 100u) % 100u;
            uint32_t dd = date % 100u;
            if (mm < 1 || mm > 12 || dd < 1 || dd > 31)
                continue;
            t.dates[t.count++] = date;
        }

        // Early closes for this exchange: "YYYYMMDD:HHMM" strings (local
        // close time). Invalid entries are skipped individually.
        t.earlyCount = 0;
        if (!earlies.isNull())
        {
            JsonArray ea = earlies[exchange].as<JsonArray>();
            if (!ea.isNull())
            {
                for (JsonVariant v : ea)
                {
                    if (t.earlyCount >= MAX_DYN_EARLY)
                        break;
                    const char *s = v.as<const char *>();
                    unsigned int d8 = 0, hhmm = 0;
                    if (!s || strlen(s) != 13 || s[8] != ':' ||
                        sscanf(s, "%8u:%4u", &d8, &hhmm) != 2)
                        continue;
                    if (d8 < 20200101u || d8 > 20991231u)
                        continue;
                    unsigned int mm = (d8 / 100u) % 100u;
                    unsigned int dd = d8 % 100u;
                    if (mm < 1 || mm > 12 || dd < 1 || dd > 31)
                        continue;
                    unsigned int hh = hhmm / 100u, mn = hhmm % 100u;
                    if (hh > 23 || mn > 59)
                        continue;
                    t.earlyDates[t.earlyCount] = d8;
                    t.earlyCloseMin[t.earlyCount] = (uint16_t)(hh * 60u + mn);
                    t.earlyCount++;
                }
            }
        }

        // An exchange with no valid dates keeps its compiled fallback instead
        if (t.count > 0)
            stagedCount++;
    }

    if (stagedCount == 0)
    {
        Log.println("Holiday JSON rejected: no usable exchange calendars");
        return false;
    }

    holidayLock();
    memcpy(dynTables, staged, sizeof(dynTables));
    dynTableCount = stagedCount;
    holidayUnlock();

    Log.println("Holiday calendars applied for " + String(stagedCount) + " exchange(s)");
    return true;
}

// Persist a good payload (prefixed with its fetch time) so the next boot
// starts from it without the network.
static void saveHolidayCache(time_t fetchedAt, const String &payload)
{
    File f = SPIFFS.open(HOLIDAY_CACHE_FILE, "w");
    if (!f)
    {
        Log.println("Failed to open holiday cache for writing");
        return;
    }
    f.println((unsigned long)fetchedAt);
    f.print(payload);
    f.close();
}

void marketHolidaysBegin()
{
    if (holidayMutex)
        return; // already initialized
    holidayMutex = xSemaphoreCreateMutex();

    File f = SPIFFS.open(HOLIDAY_CACHE_FILE, "r");
    if (!f)
    {
        Log.println("No cached holiday calendars - using compiled-in tables");
        return;
    }
    unsigned long fetchedAt = strtoul(f.readStringUntil('\n').c_str(), nullptr, 10);
    String payload = f.readString();
    f.close();

    if (applyHolidayJson(payload))
    {
        holidayLock();
        lastFetchUnix = (time_t)fetchedAt;
        holidayUnlock();
        Log.println("Loaded cached holiday calendars");
    }
}

void marketHolidaysService()
{
    static unsigned long lastCheck = 0;
    unsigned long ms = millis();
    if (lastCheck != 0 && ms - lastCheck < 60000UL)
        return; // scheduling decisions at most once a minute
    lastCheck = ms;

    time_t nowUtc = UTC.now();
    if (nowUtc < 1000000000)
        return; // clock not synced yet

    holidayLock();
    bool due = !fetchPending &&
               (lastFetchUnix == 0 || nowUtc - lastFetchUnix >= HOLIDAY_REFRESH_SECONDS) &&
               (lastAttemptMs == 0 || ms - lastAttemptMs >= HOLIDAY_RETRY_MS);
    if (due)
    {
        fetchPending = true;
        scheduledNowUtc = nowUtc;
    }
    holidayUnlock();
}

// One HTTPS fetch attempt. Runs on the core-0 task.
static bool fetchHolidayCalendars(time_t nowUtc)
{
    RenderBufferReleaseGuard renderMemory;

    WiFiClientSecure client;
    client.setInsecure(); // public holiday data - certificate pinning not worth the upkeep
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    Log.println("Fetching holiday calendars: " MARKET_HOLIDAYS_URL);
    if (!http.begin(client, MARKET_HOLIDAYS_URL))
        return false;
    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        Log.println("Holiday fetch failed, HTTP " + String(code));
        http.end();
        return false;
    }
    String payload = http.getString();
    http.end();

    if (!applyHolidayJson(payload))
        return false;
    saveHolidayCache(nowUtc, payload);
    return true;
}

void marketHolidaysTick()
{
    holidayLock();
    bool doFetch = fetchPending;
    time_t nowUtc = scheduledNowUtc;
    holidayUnlock();
    if (!doFetch)
        return;
    if (otaInProgress || WiFi.status() != WL_CONNECTED)
        return; // stays pending; retried on a later tick

    bool ok = fetchHolidayCalendars(nowUtc);

    holidayLock();
    fetchPending = false;
    lastAttemptMs = millis();
    if (ok)
        lastFetchUnix = nowUtc;
    holidayUnlock();
}

void marketHolidaysForceRefresh()
{
    time_t nowUtc = UTC.now(); // MAIN core (serial command handler)
    if (nowUtc < 1000000000)
    {
        Log.println("Clock not synced yet - cannot fetch");
        return;
    }
    holidayLock();
    fetchPending = true;
    scheduledNowUtc = nowUtc;
    lastAttemptMs = 0; // skip the failure backoff
    holidayUnlock();
    Log.println("Holiday calendar fetch queued - watch for the result above");
}

bool marketHolidaysFetchedInfo(long &ageDays)
{
    holidayLock();
    int count = dynTableCount;
    time_t fetched = lastFetchUnix;
    holidayUnlock();
    if (count == 0)
        return false;
    time_t nowUtc = UTC.now();
    ageDays = (fetched > 0 && nowUtc > fetched) ? (long)((nowUtc - fetched) / 86400)
                                                : -1;
    return true;
}

void printMarketHolidaysStatus()
{
    Log.println("=== Market holiday calendars ===");
    Log.println("Source URL: " MARKET_HOLIDAYS_URL);

    holidayLock();
    int count = dynTableCount;
    time_t fetched = lastFetchUnix;
    holidayUnlock();

    if (count == 0)
    {
        Log.println("Active data: compiled-in tables (no fetched override yet)");
    }
    else
    {
        time_t nowUtc = UTC.now();
        String age = (fetched > 0 && nowUtc > fetched)
                         ? String((long)((nowUtc - fetched) / 86400)) + " day(s) ago"
                         : "unknown";
        Log.println("Active data: fetched calendars (" + age + "), refreshed weekly");
        holidayLock();
        for (int t = 0; t < dynTableCount; t++)
        {
            Log.println("  " + String(dynTables[t].exchange) + ": " +
                           String(dynTables[t].count) + " closure dates, " +
                           String(dynTables[t].earlyCount) + " early close(s)");
        }
        holidayUnlock();
    }
    Log.println("Exchanges absent from the fetched data fall back to the compiled tables.");
}
