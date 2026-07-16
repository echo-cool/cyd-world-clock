#include "logShipper.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifdef LOG_PUSH_DISABLE

// Shipping opted out in secrets.h: no queue, no task, no RAM spent.
void logShipperFeed(const uint8_t *, size_t) {}
void logShipperBegin() {}
void logShipperService() {}
void logShipperPrintStatus(Print &out)
{
    out.println("Log shipping disabled (LOG_PUSH_DISABLE set in secrets.h)");
}

#else // !LOG_PUSH_DISABLE

// Every device ships to the project's fleet log server by default; set
// LOG_PUSH_URL in secrets.h to point at your own server (or a Loki
// instance), or define LOG_PUSH_DISABLE to compile shipping out entirely.
#ifndef LOG_PUSH_URL
#define LOG_PUSH_URL "http://esp32-clock-log-collect.echo.cool:3100/loki/api/v1/push"
#endif

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ezTime.h>

#include "ClockLogic.h" // release 38KB quadrant sprite for optional HTTPS sink
#include "genericBaseProject.h" // ntpSyncStatus
#include "deviceIdentity.h"
#include "logBuffer.h"
#include "otaUpdate.h"    // otaInProgress
#include "projectConfig.h" // hostname -> prefix for the "device" label

// Default push token for the fleet server (its AUTH_TOKEN). It authorizes
// pushing only - reading logs requires the server's separate WEB_PASSWORD -
// so baking it into a public repo costs little and keeps random scanners
// from filling the fleet's database. Override alongside LOG_PUSH_URL.
#ifndef LOG_PUSH_TOKEN
#define LOG_PUSH_TOKEN "1a7359c51e6cfe3ad4280dcac2f20e0f428b5effa1d3b580"
#endif

// Queue sizing. 6KB holds a whole boot's worth of lines (the log ring is the
// same size), so nothing from startup is lost while waiting for the first
// NTP sync. Overflow drops the OLDEST lines - the newest ones are the ones
// worth having when the network comes back.
static const size_t QUEUE_SIZE = 6144;
static const size_t SHIP_LINE_MAX = 384;          // longer lines are truncated
static const size_t BATCH_MAX = 3072;        // raw line bytes per push
static const size_t REC_HDR = 8 + 2;         // u64 line-millis + u16 length
static const uint32_t SHIP_INTERVAL_MS = 30000;
static const size_t SHIP_TRIGGER_BYTES = 2048;
static const uint32_t BACKOFF_MIN_MS = 30000;
static const uint32_t BACKOFF_MAX_MS = 600000;

// All queue state is guarded by this spinlock: logShipperFeed runs on both
// cores (any task that logs), the ship task pops on core 0, and the anchor
// is written from the main loop. Never log while holding it.
static portMUX_TYPE shipMux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t ring[QUEUE_SIZE]; // records: [u64 ms][u16 len][len bytes]
static size_t head = 0, tail = 0, used = 0;
static uint32_t queuedLines = 0;
static uint32_t dropGeneration = 0; // bumped when overflow moves the tail

// Line assembly (same "stamp at line boundary" idea as the log ring).
static char lineBuf[SHIP_LINE_MAX];
static size_t lineLen = 0;
static bool lineTruncated = false;

// Epoch anchor: epoch_ms - uptime_ms at the first real NTP sync. A line's
// absolute time is anchor + its uptime stamp, which is deterministic across
// resends - that is what makes server-side dedupe of retried batches work.
static volatile bool anchorSet = false;
static uint64_t anchorEpochMs = 0;

// Stats for the LOGSHIP command (guarded by shipMux unless noted).
static uint32_t totalShipped = 0;
static uint32_t totalDropped = 0;
static volatile int lastPushCode = 0;      // last HTTP result (0 = none yet)
static volatile uint32_t consecutiveFailures = 0;
static volatile bool taskStarted = false;

static String shipDeviceLabel; // hostname + MAC suffix snapshot (changes need a reboot)
static char bootId[9];

// millis() wraps at ~49.7 days; extend to 64 bits. Call under shipMux only
// (the wrap detection needs serialized callers).
static uint64_t millis64Locked()
{
    static uint32_t last = 0;
    static uint32_t high = 0;
    uint32_t now = millis();
    if (now < last) high++;
    last = now;
    return ((uint64_t)high << 32) | now;
}

static inline void ringPut(uint8_t c)
{
    ring[head] = c;
    head = (head + 1) % QUEUE_SIZE;
}

static inline uint8_t ringAt(size_t pos)
{
    return ring[pos % QUEUE_SIZE];
}

static size_t recordSizeAt(size_t pos)
{
    uint16_t len = ringAt(pos + 8) | ((uint16_t)ringAt(pos + 9) << 8);
    return REC_HDR + len;
}

// Callers hold shipMux.
static void dropOldestLocked()
{
    size_t sz = recordSizeAt(tail);
    tail = (tail + sz) % QUEUE_SIZE;
    used -= sz;
    queuedLines--;
    totalDropped++;
    dropGeneration++;
}

// Callers hold shipMux.
static void commitLineLocked()
{
    size_t need = REC_HDR + lineLen;
    while (used + need > QUEUE_SIZE && queuedLines > 0) dropOldestLocked();
    if (used + need > QUEUE_SIZE) return; // single line larger than the queue

    uint64_t ms = millis64Locked();
    for (int i = 0; i < 8; i++) ringPut((uint8_t)(ms >> (8 * i)));
    ringPut((uint8_t)(lineLen & 0xFF));
    ringPut((uint8_t)(lineLen >> 8));
    for (size_t i = 0; i < lineLen; i++) ringPut((uint8_t)lineBuf[i]);
    used += need;
    queuedLines++;
}

void logShipperFeed(const uint8_t *buffer, size_t size)
{
    portENTER_CRITICAL(&shipMux);
    for (size_t i = 0; i < size; i++)
    {
        char c = (char)buffer[i];
        if (c == '\r') continue;
        if (c == '\n')
        {
            if (lineLen > 0) commitLineLocked();
            lineLen = 0;
            lineTruncated = false;
            continue;
        }
        if (lineLen < SHIP_LINE_MAX)
        {
            lineBuf[lineLen++] = c;
        }
        else if (!lineTruncated)
        {
            lineTruncated = true; // keep the first SHIP_LINE_MAX bytes, drop the rest
        }
    }
    portEXIT_CRITICAL(&shipMux);
}

void logShipperService()
{
    if (anchorSet || !ntpSyncStatus) return;
    time_t epoch = UTC.now(); // main core - the only ezTime-safe place
    portENTER_CRITICAL(&shipMux);
    anchorEpochMs = (uint64_t)epoch * 1000ULL - millis64Locked();
    portEXIT_CRITICAL(&shipMux);
    anchorSet = true;
    Log.println("Log shipper: time anchored - shipping to " LOG_PUSH_URL);
}

// JSON string escaping for log lines (they can contain anything).
static void appendJsonEscaped(String &out, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        char c = s[i];
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if ((uint8_t)c < 0x20)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        }
        else
        {
            out += c;
        }
    }
}

// One push attempt. Copies a batch out under the lock, builds the Loki JSON
// push body, POSTs it, and pops the batch only after the server accepted it
// (or rejected it as malformed - retrying a poison batch forever would wedge
// the queue). Returns false only for retryable failures.
static bool shipBatch()
{
    static uint8_t batch[BATCH_MAX + SHIP_LINE_MAX + REC_HDR];

    // Peek (copy without popping): if the POST fails, the records stay
    // queued. dropGeneration tells us whether overflow moved the tail while
    // the network was busy - if so, skip the pop; the overlap is resent
    // later and the server's dedupe drops it.
    size_t copied = 0;
    uint32_t lines = 0;
    size_t newTail;
    uint32_t gen0;
    portENTER_CRITICAL(&shipMux);
    gen0 = dropGeneration;
    size_t pos = tail;
    uint32_t remaining = queuedLines;
    while (remaining > 0 && copied < BATCH_MAX)
    {
        size_t sz = recordSizeAt(pos);
        if (copied + sz > sizeof(batch)) break;
        for (size_t i = 0; i < sz; i++) batch[copied + i] = ringAt(pos + i);
        copied += sz;
        pos = (pos + sz) % QUEUE_SIZE;
        lines++;
        remaining--;
    }
    newTail = pos;
    uint64_t anchor = anchorEpochMs;
    portEXIT_CRITICAL(&shipMux);
    if (lines == 0) return true;

    bool https = strncmp(LOG_PUSH_URL, "https", 5) == 0;
    RenderBufferReleaseGuard renderMemory(https);

    String json;
    json.reserve(copied + copied / 3 + 256);
    json += "{\"streams\":[{\"stream\":{\"job\":\"cyd-world-clock\",\"device\":\"";
    json += shipDeviceLabel;
    json += "\",\"boot_id\":\"";
    json += bootId;
    json += "\"},\"values\":[";
    size_t off = 0;
    for (uint32_t n = 0; n < lines; n++)
    {
        uint64_t ms = 0;
        for (int i = 0; i < 8; i++) ms |= (uint64_t)batch[off + i] << (8 * i);
        uint16_t len = batch[off + 8] | ((uint16_t)batch[off + 9] << 8);
        char ts[24];
        snprintf(ts, sizeof(ts), "%llu", (unsigned long long)((anchor + ms) * 1000000ULL));
        if (n) json += ',';
        json += "[\"";
        json += ts;
        json += "\",\"";
        appendJsonEscaped(json, (const char *)batch + off + REC_HDR, len);
        json += "\"]";
        off += REC_HDR + len;
    }
    json += "]}]}";

    WiFiClientSecure secureClient;
    WiFiClient plainClient;
    if (https) secureClient.setInsecure(); // self-hosted log sink - pinning not worth the upkeep
    HTTPClient http;
    // Fail fast: a down or unreachable collector must never tie this ship task
    // up. Short timeouts cap a dead-server attempt at ~2-3s (the exponential
    // backoff above then spaces retries out), instead of parking the task for
    // 12s on every push while the server is down.
    http.setConnectTimeout(2000);
    http.setTimeout(3000);
    http.setReuse(false); // don't park a connection between 30s-apart pushes
    if (!http.begin(https ? secureClient : plainClient, LOG_PUSH_URL))
    {
        lastPushCode = -1;
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    if (strlen(LOG_PUSH_TOKEN) > 0)
        http.addHeader("Authorization", "Bearer " LOG_PUSH_TOKEN);
    int code = http.POST((uint8_t *)json.c_str(), json.length());
    http.end();
    lastPushCode = code;

    // 2xx = stored. Other 4xx (bad payload, bad token...) = drop the batch:
    // it would fail identically forever. 429/5xx/negative = retry later.
    bool accepted = code >= 200 && code < 300;
    bool poison = code >= 400 && code < 500 && code != HTTP_CODE_TOO_MANY_REQUESTS;
    if (!accepted && !poison) return false;

    portENTER_CRITICAL(&shipMux);
    if (dropGeneration == gen0)
    {
        tail = newTail;
        used -= copied;
        queuedLines -= lines;
    }
    if (accepted) totalShipped += lines;
    portEXIT_CRITICAL(&shipMux);
    return accepted;
}

static void logShipperTask(void *)
{
    uint64_t nextAttemptMs = 0;
    for (;;)
    {
        delay(1000);
        if (otaInProgress) continue; // give OTA the radio and the CPU
        if (WiFi.status() != WL_CONNECTED) continue;
        if (!anchorSet) continue; // no real time yet - keep queueing

        portENTER_CRITICAL(&shipMux);
        uint64_t nowMs = millis64Locked();
        size_t bytes = used;
        uint64_t oldestMs = 0;
        if (queuedLines > 0)
        {
            for (int i = 0; i < 8; i++) oldestMs |= (uint64_t)ringAt(tail + i) << (8 * i);
        }
        portEXIT_CRITICAL(&shipMux);

        if (bytes == 0 || nowMs < nextAttemptMs) continue;
        if (bytes < SHIP_TRIGGER_BYTES && nowMs - oldestMs < SHIP_INTERVAL_MS) continue;

        if (shipBatch())
        {
            if (consecutiveFailures > 0)
                Log.println("Log shipper: push recovered (HTTP " + String(lastPushCode) + ")");
            consecutiveFailures = 0;
            nextAttemptMs = 0;
        }
        else
        {
            consecutiveFailures = consecutiveFailures + 1;
            uint32_t shift = consecutiveFailures - 1;
            if (shift > 4) shift = 4;
            uint32_t backoff = BACKOFF_MIN_MS << shift;
            if (backoff > BACKOFF_MAX_MS) backoff = BACKOFF_MAX_MS;
            nextAttemptMs = nowMs + backoff;
            // First failure only (plus a reminder every 10th): a down server
            // must not turn into a log-about-logging feedback loop.
            if (consecutiveFailures == 1 || consecutiveFailures % 10 == 0)
                Log.println("Log shipper: push failed (" + String(lastPushCode) +
                            "), retrying in " + String(backoff / 1000) + "s");
        }
    }
}

void logShipperBegin()
{
    if (taskStarted) return;
    taskStarted = true;
    shipDeviceLabel = deviceLabel();
    snprintf(bootId, sizeof(bootId), "%08x", (unsigned)esp_random());
    Log.println("Log shipper: enabled, device \"" + shipDeviceLabel + "\" boot " +
                bootId + " -> " LOG_PUSH_URL);
    // Core 0 (the Arduino loop runs on core 1). 16KB stack: HTTPS through
    // WiFiClientSecure needs far more headroom than the FreeRTOS default.
    xTaskCreatePinnedToCore(logShipperTask, "logship", 16384, nullptr, 1, nullptr, 0);
}

void logShipperPrintStatus(Print &out)
{
    portENTER_CRITICAL(&shipMux);
    uint32_t queued = queuedLines;
    size_t bytes = used;
    uint32_t dropped = totalDropped;
    uint32_t shipped = totalShipped;
    portEXIT_CRITICAL(&shipMux);

    out.println("=== Log shipper ===");
    out.println("Target:   " LOG_PUSH_URL);
    out.println("Device:   " + shipDeviceLabel + " (boot " + bootId + ")");
    out.println("State:    " + String(!taskStarted ? "not started"
                                      : !anchorSet ? "waiting for first NTP sync"
                                                   : "shipping"));
    out.println("Queued:   " + String(queued) + " lines (" + String(bytes) + " bytes)");
    out.println("Shipped:  " + String(shipped) + " lines, dropped " + String(dropped));
    out.println("Last push: HTTP " + String(lastPushCode) +
                (consecutiveFailures ? " (" + String(consecutiveFailures) + " consecutive failures)"
                                     : String("")));
}

#endif // !LOG_PUSH_DISABLE
