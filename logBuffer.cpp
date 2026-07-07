#include "logBuffer.h"
#include "logShipper.h" // optional remote log push (no-op unless configured)

LogBuffer Log;

static const size_t LOG_RING_SIZE = 6144;
static char ring[LOG_RING_SIZE];
static size_t head = 0; // next write position
static bool wrapped = false;
static bool atLineStart = true;
static volatile uint32_t lineVersion = 0;

// Spinlock, not a FreeRTOS mutex: writes come from both cores and must also
// work before the logger has been "begun" (there is no begin - static init
// covers everything).
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

static inline void ringPut(char c)
{
    ring[head] = c;
    head = (head + 1) % LOG_RING_SIZE;
    if (head == 0) wrapped = true;
}

// Callers hold logMux. CRs are dropped and each line gets an uptime stamp so
// entries can be correlated without a serial monitor attached.
static void ringWrite(const uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        char c = (char)buffer[i];
        if (c == '\r') continue;
        if (atLineStart && c != '\n')
        {
            char stamp[14];
            unsigned long s = millis() / 1000UL;
            snprintf(stamp, sizeof(stamp), "[%02lu:%02lu:%02lu] ",
                     (s / 3600UL) % 100UL, (s / 60UL) % 60UL, s % 60UL);
            for (const char *p = stamp; *p; p++) ringPut(*p);
            atLineStart = false;
        }
        if (c == '\n')
        {
            atLineStart = true;
            lineVersion = lineVersion + 1;
        }
        ringPut(c);
    }
}

size_t LogBuffer::write(uint8_t c)
{
    return write(&c, 1);
}

size_t LogBuffer::write(const uint8_t *buffer, size_t size)
{
    // Serial first (it may block on a full TX buffer - do that outside the
    // spinlock), then the ring.
    size_t n = Serial.write(buffer, size);
    portENTER_CRITICAL(&logMux);
    ringWrite(buffer, size);
    portEXIT_CRITICAL(&logMux);
    // Tee into the remote-shipping queue (its own lock; never nested with
    // logMux, and a no-op when LOG_PUSH_URL isn't configured).
    logShipperFeed(buffer, size);
    return n;
}

uint32_t logVersion()
{
    return lineVersion; // 32-bit reads are atomic on the ESP32
}

String logTail(size_t maxBytes)
{
    if (maxBytes > LOG_RING_SIZE) maxBytes = LOG_RING_SIZE;

    // Allocate outside the critical section (heap ops must not run with
    // interrupts disabled), memcpy inside it.
    char *tmp = (char *)malloc(maxBytes + 1);
    if (!tmp) return String();

    portENTER_CRITICAL(&logMux);
    size_t avail = wrapped ? LOG_RING_SIZE : head;
    size_t want = (maxBytes < avail) ? maxBytes : avail;
    size_t start = (head + LOG_RING_SIZE - want) % LOG_RING_SIZE;
    for (size_t i = 0; i < want; i++)
    {
        tmp[i] = ring[(start + i) % LOG_RING_SIZE];
    }
    bool clipped = (want < avail) || wrapped;
    portEXIT_CRITICAL(&logMux);
    tmp[want] = '\0';

    // If the snapshot starts mid-line, drop the partial first line.
    char *text = tmp;
    if (clipped)
    {
        char *nl = strchr(tmp, '\n');
        if (nl && *(nl + 1) != '\0') text = nl + 1;
    }

    String out(text);
    free(tmp);
    return out;
}
