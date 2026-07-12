#ifndef TIMER_LOGIC_H
#define TIMER_LOGIC_H

// ---------------------------------------------------------------------------
// Stopwatch / countdown state machines for the two timer clock faces, split
// out of the display code so they are unit-tested on the host
// (test/test_timerlogic) without pulling in Arduino or the TFT.
//
// Pure: every function takes the current MONOTONIC timestamp in milliseconds
// (on the device: esp_timer_get_time() / 1000 - 64-bit, never wraps, and
// unaffected by NTP stepping the wall clock). Nothing here reads a clock,
// draws, logs or touches the filesystem - the device integration lives in
// timerFaces.cpp.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace timerlogic
{

enum TimerPhase : uint8_t
{
    TIMER_READY = 0, // reset/stopped, showing 0 / the configured duration
    TIMER_RUNNING,
    TIMER_PAUSED,
    TIMER_FINISHED, // countdown only: reached 00:00:00
};

// "READY" / "RUNNING" / "PAUSED" / "FINISHED" - shared by the faces and
// /api/status.
const char *timerPhaseName(TimerPhase phase);

// Countdown duration limits: 1 minute to 99:59:59.
const uint64_t COUNTDOWN_MIN_MS = 60ULL * 1000ULL;
const uint64_t COUNTDOWN_MAX_MS =
    (99ULL * 3600ULL + 59ULL * 60ULL + 59ULL) * 1000ULL;
const uint64_t COUNTDOWN_DEFAULT_MS = 30ULL * 60ULL * 1000ULL;

// Milestone reminder interval limits (minutes); anything outside the range
// disables the reminders (pollMilestoneMinutes never fires).
const int REMINDER_MIN_MINUTES = 1;
const int REMINDER_MAX_MINUTES = 1440;

uint64_t clampCountdownMs(int64_t ms);
bool reminderIntervalValid(int minutes);

// Count-up timer. Elapsed time keeps counting past 24 hours (64-bit ms);
// paused spans are excluded via the accumulator.
struct Stopwatch
{
    TimerPhase phase = TIMER_READY;
    uint64_t accumMs = 0; // elapsed accumulated across previous run spans
    uint64_t startMs = 0; // monotonic timestamp of the last start/resume
    // Milestone bookkeeping: interval multiples already notified, and the
    // interval they were counted against (a changed interval rebases instead
    // of replaying old boundaries).
    uint64_t firedCount = 0;
    int firedIntervalMin = 0;

    bool start(uint64_t nowMs);  // READY -> RUNNING
    bool pause(uint64_t nowMs);  // RUNNING -> PAUSED
    bool resume(uint64_t nowMs); // PAUSED -> RUNNING
    void reset();                // any -> READY at 0
    uint64_t elapsedMs(uint64_t nowMs) const;

    // Milestone poll: returns the minute mark (30, 60, 90, ...) newly reached
    // since the last call, or 0. Each boundary fires exactly once; when a
    // delayed update skips over several boundaries only the most recent one
    // is reported (no replay storm). Invalid intervals never fire.
    uint32_t pollMilestoneMinutes(uint64_t nowMs, int intervalMinutes);
};

// Count-down timer. The configured duration is kept across runs: reset
// restores it, and start after FINISHED restarts the same duration.
struct Countdown
{
    TimerPhase phase = TIMER_READY;
    uint64_t durationMs = COUNTDOWN_DEFAULT_MS; // configured duration
    uint64_t endMs = 0;                         // deadline while RUNNING
    uint64_t remainMs = COUNTDOWN_DEFAULT_MS;   // frozen remaining otherwise
    uint64_t prevRemainMs = 0; // milestone tracking (last remaining seen)

    // Duration edits are only allowed while stopped (READY / FINISHED) and
    // are clamped to [COUNTDOWN_MIN_MS, COUNTDOWN_MAX_MS]. An edit in the
    // FINISHED state returns the timer to READY at the new duration.
    bool setDurationMs(int64_t ms);
    bool adjustDurationMs(int64_t deltaMs);

    bool start(uint64_t nowMs);  // READY/FINISHED -> RUNNING at full duration
    bool pause(uint64_t nowMs);  // RUNNING -> PAUSED
    bool resume(uint64_t nowMs); // PAUSED -> RUNNING
    void reset();                // any -> READY at the configured duration

    // Remaining time, clamped at 0 - never negative however late the poll is.
    uint64_t remainingMs(uint64_t nowMs) const;

    // True exactly once, on the poll that finds the running countdown at 0
    // (phase moves to FINISHED). The caller raises the final alarm.
    bool pollFinished(uint64_t nowMs);

    // Milestone poll: returns the remaining-minutes mark (…, 90, 60, 30)
    // newly crossed since the last call, or 0. Boundaries are the positive
    // multiples of the interval strictly below the starting duration, so a
    // 100-minute countdown with a 30-minute interval fires at 90/60/30 and a
    // 120-minute one at 90/60/30 (not 120). Zero never fires here - the
    // final alarm owns it. Skipped boundaries collapse into the most recent.
    uint32_t pollMilestoneMinutes(uint64_t nowMs, int intervalMinutes);
};

// "HH:MM:SS", zero-padded; hours grow beyond two digits without wrapping
// ("125:04:09"). buf should hold at least 16 bytes.
void formatHMS(uint64_t totalSeconds, char *buf, size_t bufLen);

// "HH:MM" from whole minutes, same no-wrap hour behavior ("125:04"). Used by
// the seconds-hidden (focus) display on the timer faces.
void formatHM(uint64_t totalMinutes, char *buf, size_t bufLen);

// Whole-minute values for the seconds-hidden display: elapsed floors (shows
// 00:00 through the first minute), remaining ceils (shows the full duration
// at start and never reads 00:00 while any time is actually left).
uint64_t displayMinutesElapsed(uint64_t elapsedMs);
uint64_t displayMinutesRemaining(uint64_t remainingMs);

} // namespace timerlogic

#endif // TIMER_LOGIC_H
