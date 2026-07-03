#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

// ---------------------------------------------------------------------------
// Project logger: a Print-compatible object that tees everything to the
// serial port AND into an in-RAM ring buffer, so the most recent log lines
// can be read on the device (settings -> Logs) and in a browser (/logs)
// without a USB cable.
//
// The project's code logs through the global `Log` instead of `Serial`
// (serial *input* - the command interface - still uses Serial directly).
// Lines in the ring get an uptime timestamp prefix; the serial byte stream
// is forwarded unmodified. Writers run on both cores (main loop and the
// core-0 fetch task), so the ring is guarded by a spinlock.
// ---------------------------------------------------------------------------

#include <Arduino.h>

class LogBuffer : public Print
{
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
};

extern LogBuffer Log;

// Snapshot of the newest part of the log (up to maxBytes of it), oldest
// line first, starting at a line boundary. Allocates transiently; call from
// page-render / web-handler context, not from tight loops.
String logTail(size_t maxBytes);

// Bumped whenever a full line lands in the ring; the log pages poll this to
// know when a repaint is worth it.
uint32_t logVersion();

#endif // LOG_BUFFER_H
