#include "timerLogic.h"

#include <cstdio>

namespace timerlogic
{

const char *timerPhaseName(TimerPhase phase)
{
    switch (phase) {
    case TIMER_RUNNING: return "RUNNING";
    case TIMER_PAUSED: return "PAUSED";
    case TIMER_FINISHED: return "FINISHED";
    default: return "READY";
    }
}

uint64_t clampCountdownMs(int64_t ms)
{
    if (ms < (int64_t)COUNTDOWN_MIN_MS) return COUNTDOWN_MIN_MS;
    if (ms > (int64_t)COUNTDOWN_MAX_MS) return COUNTDOWN_MAX_MS;
    return (uint64_t)ms;
}

bool reminderIntervalValid(int minutes)
{
    return minutes >= REMINDER_MIN_MINUTES && minutes <= REMINDER_MAX_MINUTES;
}

/*-------- Stopwatch ----------*/

bool Stopwatch::start(uint64_t nowMs)
{
    if (phase != TIMER_READY) return false;
    accumMs = 0;
    startMs = nowMs;
    firedCount = 0;
    phase = TIMER_RUNNING;
    return true;
}

bool Stopwatch::pause(uint64_t nowMs)
{
    if (phase != TIMER_RUNNING) return false;
    accumMs += nowMs - startMs;
    phase = TIMER_PAUSED;
    return true;
}

bool Stopwatch::resume(uint64_t nowMs)
{
    if (phase != TIMER_PAUSED) return false;
    startMs = nowMs;
    phase = TIMER_RUNNING;
    return true;
}

void Stopwatch::reset()
{
    phase = TIMER_READY;
    accumMs = 0;
    startMs = 0;
    firedCount = 0;
}

uint64_t Stopwatch::elapsedMs(uint64_t nowMs) const
{
    if (phase == TIMER_RUNNING) return accumMs + (nowMs - startMs);
    return accumMs; // READY: 0, PAUSED: frozen
}

uint32_t Stopwatch::pollMilestoneMinutes(uint64_t nowMs, int intervalMinutes)
{
    if (phase == TIMER_READY || !reminderIntervalValid(intervalMinutes)) return 0;
    uint64_t intervalMs = (uint64_t)intervalMinutes * 60000ULL;
    uint64_t k = elapsedMs(nowMs) / intervalMs;

    // Interval changed mid-run (web settings): rebase so old boundaries are
    // not replayed against the new spacing; the next NEW boundary fires.
    if (intervalMinutes != firedIntervalMin) {
        firedIntervalMin = intervalMinutes;
        firedCount = k;
        return 0;
    }

    if (k <= firedCount) return 0;
    firedCount = k; // several boundaries skipped -> report only the latest
    return (uint32_t)(k * (uint64_t)intervalMinutes);
}

/*-------- Countdown ----------*/

bool Countdown::setDurationMs(int64_t ms)
{
    if (phase == TIMER_RUNNING || phase == TIMER_PAUSED) return false;
    durationMs = clampCountdownMs(ms);
    remainMs = durationMs;
    phase = TIMER_READY; // an edit after FINISHED re-arms the timer
    return true;
}

bool Countdown::adjustDurationMs(int64_t deltaMs)
{
    return setDurationMs((int64_t)durationMs + deltaMs);
}

bool Countdown::start(uint64_t nowMs)
{
    if (phase == TIMER_RUNNING || phase == TIMER_PAUSED) return false;
    endMs = nowMs + durationMs;
    remainMs = durationMs;
    prevRemainMs = durationMs; // boundaries strictly below the start fire
    phase = TIMER_RUNNING;
    return true;
}

bool Countdown::pause(uint64_t nowMs)
{
    if (phase != TIMER_RUNNING) return false;
    remainMs = remainingMs(nowMs);
    phase = TIMER_PAUSED;
    return true;
}

bool Countdown::resume(uint64_t nowMs)
{
    if (phase != TIMER_PAUSED) return false;
    endMs = nowMs + remainMs;
    phase = TIMER_RUNNING;
    return true;
}

void Countdown::reset()
{
    phase = TIMER_READY;
    remainMs = durationMs;
    endMs = 0;
    prevRemainMs = 0;
}

uint64_t Countdown::remainingMs(uint64_t nowMs) const
{
    if (phase == TIMER_RUNNING) return endMs > nowMs ? endMs - nowMs : 0;
    return remainMs; // READY: full duration, PAUSED: frozen, FINISHED: 0
}

bool Countdown::pollFinished(uint64_t nowMs)
{
    if (phase != TIMER_RUNNING) return false;
    if (remainingMs(nowMs) > 0) return false;
    phase = TIMER_FINISHED;
    remainMs = 0;
    prevRemainMs = 0;
    return true;
}

uint32_t Countdown::pollMilestoneMinutes(uint64_t nowMs, int intervalMinutes)
{
    if (phase != TIMER_RUNNING || !reminderIntervalValid(intervalMinutes)) return 0;
    uint64_t intervalMs = (uint64_t)intervalMinutes * 60000ULL;
    uint64_t rem = remainingMs(nowMs);
    uint64_t prev = prevRemainMs;
    prevRemainMs = rem;

    if (rem == 0) return 0; // the final alarm owns zero

    // Smallest boundary at/above the current remaining; it fired iff it was
    // still strictly below the previously seen remaining. Boundaries skipped
    // by a delayed update all sit between rem and prev, so only this most
    // recent one is reported and the older ones can never replay.
    uint64_t m = (rem + intervalMs - 1) / intervalMs;
    if (m >= 1 && m * intervalMs < prev) {
        return (uint32_t)(m * (uint64_t)intervalMinutes);
    }
    return 0;
}

/*-------- Formatting ----------*/

void formatHMS(uint64_t totalSeconds, char *buf, size_t bufLen)
{
    unsigned long long h = totalSeconds / 3600ULL;
    unsigned m = (unsigned)((totalSeconds / 60ULL) % 60ULL);
    unsigned s = (unsigned)(totalSeconds % 60ULL);
    snprintf(buf, bufLen, "%02llu:%02u:%02u", h, m, s);
}

} // namespace timerlogic
