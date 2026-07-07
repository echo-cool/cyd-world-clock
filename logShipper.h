#ifndef LOG_SHIPPER_H
#define LOG_SHIPPER_H

// ---------------------------------------------------------------------------
// Remote log shipping: pushes the device log (the same lines the LogBuffer
// ring holds) to a central server over HTTP, in Grafana Loki's JSON push
// format - so the target can be the companion cyd-world-clock-logs server
// (github.com/echo-cool/cyd-world-clock-logs) or a real Loki instance,
// interchangeably.
//
// Enabled by defining LOG_PUSH_URL in secrets.h (see secrets.h.example);
// without it every function here compiles to a no-op and no RAM is spent.
//
// How it works: LogBuffer::write tees complete lines into a drop-oldest
// queue (logShipperFeed). A core-0 task batches queued lines every ~30s
// (sooner when the queue fills) and POSTs them with labels
// {job, device=<hostname>, boot_id=<random per boot>}. Timestamps come from
// an epoch anchor captured on the first real NTP sync, so boot lines queued
// before the sync still get correct absolute times retroactively - nothing
// ships until that first sync (no internet means nothing could ship anyway).
// Batches are resent on failure; the server deduplicates on (stream,
// timestamp, line), so retries and overlap after queue overflow are safe.
// ---------------------------------------------------------------------------

#include <Arduino.h>

// Tee raw log bytes into the ship queue. Called by LogBuffer::write on both
// cores; cheap (spinlock + memcpy), never blocks on the network.
void logShipperFeed(const uint8_t *buffer, size_t size);

// Start the shipping task (call once WiFi/services are starting; queued
// boot lines are kept and shipped after the first NTP sync).
void logShipperBegin();

// Call from the main loop: captures the epoch anchor once NTP has really
// synced (ezTime is not thread-safe, so the time is read on the main core).
void logShipperService();

// Human-readable status for the LOGSHIP serial command.
void logShipperPrintStatus(Print &out);

#endif // LOG_SHIPPER_H
