// Host unit tests for the stopwatch/countdown engines (timerLogic.cpp) used
// by the timer clock faces. All timestamps are simulated monotonic
// milliseconds, so every test is deterministic.

#include <unity.h>

#include "timerLogic.h"

using namespace timerlogic;

void setUp() {}
void tearDown() {}

static const uint64_t MIN_MS = 60000ULL;
static const uint64_t HOUR_MS = 3600ULL * 1000ULL;

/*-------- Stopwatch ----------*/

void test_stopwatch_start_and_elapsed()
{
    Stopwatch sw;
    TEST_ASSERT_EQUAL(TIMER_READY, sw.phase);
    TEST_ASSERT_EQUAL_UINT64(0, sw.elapsedMs(5000));

    TEST_ASSERT_TRUE(sw.start(1000));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, sw.phase);
    TEST_ASSERT_EQUAL_UINT64(0, sw.elapsedMs(1000));
    TEST_ASSERT_EQUAL_UINT64(61000 - 1000, sw.elapsedMs(61000));

    // start() while already running is rejected and does not reset anything
    TEST_ASSERT_FALSE(sw.start(70000));
    TEST_ASSERT_EQUAL_UINT64(69000, sw.elapsedMs(70000));
}

void test_stopwatch_pause_and_resume()
{
    Stopwatch sw;
    sw.start(0);
    TEST_ASSERT_TRUE(sw.pause(10000)); // ran 10s
    TEST_ASSERT_EQUAL(TIMER_PAUSED, sw.phase);

    // Frozen while paused, however long the pause lasts
    TEST_ASSERT_EQUAL_UINT64(10000, sw.elapsedMs(10000));
    TEST_ASSERT_EQUAL_UINT64(10000, sw.elapsedMs(500000));

    TEST_ASSERT_TRUE(sw.resume(500000));
    // 10s before the pause + 5s after: the 490s pause is excluded
    TEST_ASSERT_EQUAL_UINT64(15000, sw.elapsedMs(505000));

    // pause() when not running / resume() when not paused are rejected
    TEST_ASSERT_FALSE(sw.resume(505000));
    TEST_ASSERT_TRUE(sw.pause(505000));
    TEST_ASSERT_FALSE(sw.pause(505000));
}

void test_stopwatch_reset()
{
    Stopwatch sw;
    sw.start(0);
    sw.pollMilestoneMinutes(0, 30);
    TEST_ASSERT_EQUAL_UINT64(45 * MIN_MS, sw.elapsedMs(45 * MIN_MS));

    sw.reset(); // reset while running: stop and return to zero
    TEST_ASSERT_EQUAL(TIMER_READY, sw.phase);
    TEST_ASSERT_EQUAL_UINT64(0, sw.elapsedMs(46 * MIN_MS));

    // A fresh run does not inherit milestone state from the previous one
    sw.start(100 * MIN_MS);
    TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(100 * MIN_MS + 1000, 30));
}

void test_stopwatch_beyond_24_hours()
{
    Stopwatch sw;
    sw.start(0);
    uint64_t now = 30ULL * 24ULL * HOUR_MS + 2 * HOUR_MS; // 30 days 2 hours
    TEST_ASSERT_EQUAL_UINT64(now, sw.elapsedMs(now));

    char buf[16];
    formatHMS(sw.elapsedMs(now) / 1000ULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("722:00:00", buf); // no 24h wrap
}

/*-------- Countdown ----------*/

void test_countdown_start_and_remaining()
{
    Countdown cd;
    TEST_ASSERT_EQUAL_UINT64(30 * MIN_MS, cd.durationMs); // 30 min default
    TEST_ASSERT_EQUAL_UINT64(30 * MIN_MS, cd.remainingMs(0));

    TEST_ASSERT_TRUE(cd.start(1000));
    TEST_ASSERT_EQUAL(TIMER_RUNNING, cd.phase);
    TEST_ASSERT_EQUAL_UINT64(30 * MIN_MS, cd.remainingMs(1000));
    TEST_ASSERT_EQUAL_UINT64(29 * MIN_MS, cd.remainingMs(1000 + MIN_MS));
}

void test_countdown_pause_and_resume()
{
    Countdown cd;
    cd.setDurationMs(10 * MIN_MS);
    cd.start(0);
    TEST_ASSERT_TRUE(cd.pause(4 * MIN_MS)); // 6 min left
    TEST_ASSERT_EQUAL_UINT64(6 * MIN_MS, cd.remainingMs(4 * MIN_MS));
    TEST_ASSERT_EQUAL_UINT64(6 * MIN_MS, cd.remainingMs(999 * MIN_MS)); // frozen

    TEST_ASSERT_TRUE(cd.resume(1000 * MIN_MS));
    TEST_ASSERT_EQUAL_UINT64(5 * MIN_MS, cd.remainingMs(1001 * MIN_MS));

    // Duration edits are rejected while running or paused
    cd.pause(1001 * MIN_MS);
    TEST_ASSERT_FALSE(cd.setDurationMs(20 * MIN_MS));
    TEST_ASSERT_FALSE(cd.adjustDurationMs(5 * MIN_MS));
    TEST_ASSERT_EQUAL_UINT64(10 * MIN_MS, cd.durationMs);
}

void test_countdown_reaches_exactly_zero()
{
    Countdown cd;
    cd.setDurationMs(2 * MIN_MS);
    cd.start(1000);

    TEST_ASSERT_FALSE(cd.pollFinished(1000 + 2 * MIN_MS - 1));
    TEST_ASSERT_EQUAL_UINT64(1, cd.remainingMs(1000 + 2 * MIN_MS - 1));

    // Fires exactly once, at (or after) the deadline
    TEST_ASSERT_TRUE(cd.pollFinished(1000 + 2 * MIN_MS));
    TEST_ASSERT_EQUAL(TIMER_FINISHED, cd.phase);
    TEST_ASSERT_EQUAL_UINT64(0, cd.remainingMs(1000 + 2 * MIN_MS));
    TEST_ASSERT_FALSE(cd.pollFinished(1000 + 3 * MIN_MS));
}

void test_countdown_never_negative()
{
    Countdown cd;
    cd.setDurationMs(MIN_MS);
    cd.start(0);
    // A very late poll (loop stalled) still clamps at zero
    TEST_ASSERT_EQUAL_UINT64(0, cd.remainingMs(50 * HOUR_MS));
    TEST_ASSERT_TRUE(cd.pollFinished(50 * HOUR_MS));
    TEST_ASSERT_EQUAL_UINT64(0, cd.remainingMs(51 * HOUR_MS));
}

void test_countdown_reset_restores_duration()
{
    Countdown cd;
    cd.setDurationMs(45 * MIN_MS);
    cd.start(0);
    cd.reset(); // mid-run
    TEST_ASSERT_EQUAL(TIMER_READY, cd.phase);
    TEST_ASSERT_EQUAL_UINT64(45 * MIN_MS, cd.remainingMs(10 * MIN_MS));

    // After completion: reset restores the configured duration, and a
    // restart runs the same duration again.
    cd.start(0);
    cd.pollFinished(46 * MIN_MS);
    TEST_ASSERT_EQUAL(TIMER_FINISHED, cd.phase);
    cd.reset();
    TEST_ASSERT_EQUAL_UINT64(45 * MIN_MS, cd.remainingMs(50 * MIN_MS));
    TEST_ASSERT_TRUE(cd.start(100 * MIN_MS)); // restart after finish
    TEST_ASSERT_EQUAL_UINT64(45 * MIN_MS, cd.remainingMs(100 * MIN_MS));
}

void test_countdown_duration_clamping()
{
    Countdown cd;
    cd.setDurationMs(-5000); // below 1 minute
    TEST_ASSERT_EQUAL_UINT64(COUNTDOWN_MIN_MS, cd.durationMs);

    cd.setDurationMs((int64_t)1000ULL * HOUR_MS); // above 99:59:59
    TEST_ASSERT_EQUAL_UINT64(COUNTDOWN_MAX_MS, cd.durationMs);

    // Repeated -30 min taps at the floor stay clamped, never underflow
    cd.setDurationMs(5 * MIN_MS);
    cd.adjustDurationMs(-30LL * (int64_t)MIN_MS);
    cd.adjustDurationMs(-30LL * (int64_t)MIN_MS);
    TEST_ASSERT_EQUAL_UINT64(COUNTDOWN_MIN_MS, cd.durationMs);
    TEST_ASSERT_EQUAL_UINT64(COUNTDOWN_MIN_MS, cd.remainingMs(0));
}

/*-------- Milestones ----------*/

void test_stopwatch_milestone_detection()
{
    Stopwatch sw;
    sw.start(0);
    sw.pollMilestoneMinutes(0, 30); // arm at elapsed 0

    TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(29 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(30, sw.pollMilestoneMinutes(30 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(31 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(60, sw.pollMilestoneMinutes(60 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(90, sw.pollMilestoneMinutes(90 * MIN_MS, 30));
}

void test_countdown_milestone_detection()
{
    Countdown cd;
    cd.setDurationMs(2 * 60 * MIN_MS); // 2 hours, 30-minute interval
    cd.start(0);

    // The starting boundary (120 = full duration) must NOT fire
    TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(1, 30));
    TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(29 * MIN_MS, 30));
    // 1:30 / 1:00 / 0:30 remaining
    TEST_ASSERT_EQUAL_UINT32(90, cd.pollMilestoneMinutes(30 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(31 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(60, cd.pollMilestoneMinutes(60 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(30, cd.pollMilestoneMinutes(90 * MIN_MS, 30));
    // Zero remaining never fires the small reminder (final alarm owns it)
    TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(120 * MIN_MS, 30));
}

void test_no_duplicate_milestones()
{
    Stopwatch sw;
    sw.start(0);
    sw.pollMilestoneMinutes(0, 30);
    TEST_ASSERT_EQUAL_UINT32(30, sw.pollMilestoneMinutes(30 * MIN_MS, 30));
    for (int i = 0; i < 50; i++) { // repeated polls on the same boundary
        TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(30 * MIN_MS + i, 30));
    }

    Countdown cd;
    cd.setDurationMs(60 * MIN_MS);
    cd.start(0);
    TEST_ASSERT_EQUAL_UINT32(30, cd.pollMilestoneMinutes(30 * MIN_MS, 30));
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(30 * MIN_MS + i, 30));
    }
}

void test_milestone_crossed_by_late_update()
{
    // The loop can stall (network hiccup); a poll that lands well past the
    // boundary must still fire it exactly once.
    Stopwatch sw;
    sw.start(0);
    sw.pollMilestoneMinutes(0, 30);
    TEST_ASSERT_EQUAL_UINT32(30, sw.pollMilestoneMinutes(30 * MIN_MS + 47000, 30));
    TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(31 * MIN_MS, 30));

    Countdown cd;
    cd.setDurationMs(60 * MIN_MS);
    cd.start(0);
    TEST_ASSERT_EQUAL_UINT32(30, cd.pollMilestoneMinutes(30 * MIN_MS + 47000, 30));
    TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(31 * MIN_MS, 30));
}

void test_multiple_milestones_in_one_update()
{
    // A very delayed update skips several boundaries: only the most recent
    // one is reported (no replay storm), and none of them fire again.
    Stopwatch sw;
    sw.start(0);
    sw.pollMilestoneMinutes(0, 30);
    TEST_ASSERT_EQUAL_UINT32(90, sw.pollMilestoneMinutes(95 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(96 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(120, sw.pollMilestoneMinutes(120 * MIN_MS, 30));

    Countdown cd;
    cd.setDurationMs(100 * MIN_MS);
    cd.start(0);
    // remaining jumps 100 -> 25 min: crossed 90, 60 and 30; only 30 fires
    TEST_ASSERT_EQUAL_UINT32(30, cd.pollMilestoneMinutes(75 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(76 * MIN_MS, 30));
}

void test_countdown_non_multiple_duration()
{
    // 100-minute countdown, 30-minute interval: reminders at 90/60/30
    // remaining - the boundary at/above the duration never fires.
    Countdown cd;
    cd.setDurationMs(100 * MIN_MS);
    cd.start(0);
    TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(9 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(90, cd.pollMilestoneMinutes(10 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(60, cd.pollMilestoneMinutes(40 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(30, cd.pollMilestoneMinutes(70 * MIN_MS, 30));
    TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(99 * MIN_MS, 30));
}

void test_invalid_interval_disables_milestones()
{
    TEST_ASSERT_FALSE(reminderIntervalValid(0));
    TEST_ASSERT_FALSE(reminderIntervalValid(-5));
    TEST_ASSERT_FALSE(reminderIntervalValid(1441));
    TEST_ASSERT_TRUE(reminderIntervalValid(1));
    TEST_ASSERT_TRUE(reminderIntervalValid(1440));

    Stopwatch sw;
    sw.start(0);
    TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(90 * MIN_MS, 0));
    TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(91 * MIN_MS, -5));
    TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(92 * MIN_MS, 1441));

    Countdown cd;
    cd.setDurationMs(120 * MIN_MS);
    cd.start(0);
    TEST_ASSERT_EQUAL_UINT32(0, cd.pollMilestoneMinutes(60 * MIN_MS, 0));

    // A 1-minute interval (range floor) works
    Stopwatch sw2;
    sw2.start(0);
    sw2.pollMilestoneMinutes(0, 1);
    TEST_ASSERT_EQUAL_UINT32(1, sw2.pollMilestoneMinutes(MIN_MS, 1));
}

void test_timer_formatting()
{
    char buf[16];
    formatHMS(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("00:00:00", buf);
    formatHMS(59, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("00:00:59", buf);
    formatHMS(2 * 3600 + 15 * 60 + 32, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("02:15:32", buf);
    formatHMS(125ULL * 3600 + 4 * 60 + 9, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("125:04:09", buf); // hours never wrap
    formatHMS(99ULL * 3600 + 59 * 60 + 59, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("99:59:59", buf);
}

void test_hidden_seconds_display()
{
    // Focus mode shows whole minutes: elapsed floors, remaining ceils.
    char buf[16];
    formatHM(0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("00:00", buf);
    formatHM(30, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("00:30", buf);
    formatHM(125ULL * 60 + 4, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("125:04", buf); // hours never wrap here either

    // Elapsed floors: 00:00 through the whole first minute
    TEST_ASSERT_EQUAL_UINT64(0, displayMinutesElapsed(0));
    TEST_ASSERT_EQUAL_UINT64(0, displayMinutesElapsed(59999));
    TEST_ASSERT_EQUAL_UINT64(1, displayMinutesElapsed(60000));

    // Remaining ceils: full duration at start, never 00:00 while time is left
    TEST_ASSERT_EQUAL_UINT64(30, displayMinutesRemaining(30 * MIN_MS));
    TEST_ASSERT_EQUAL_UINT64(30, displayMinutesRemaining(29 * MIN_MS + 1));
    TEST_ASSERT_EQUAL_UINT64(29, displayMinutesRemaining(29 * MIN_MS));
    TEST_ASSERT_EQUAL_UINT64(1, displayMinutesRemaining(1));
    TEST_ASSERT_EQUAL_UINT64(0, displayMinutesRemaining(0)); // finished only
}

void test_64bit_timestamps_long_running()
{
    // Timestamps far beyond the 32-bit millis() wrap (49.7 days): a base of
    // ~500 days plus a 60-day run must stay exact.
    const uint64_t base = 500ULL * 24ULL * HOUR_MS;
    Stopwatch sw;
    sw.start(base);
    uint64_t now = base + 60ULL * 24ULL * HOUR_MS + 12345ULL;
    TEST_ASSERT_EQUAL_UINT64(60ULL * 24ULL * HOUR_MS + 12345ULL, sw.elapsedMs(now));

    char buf[16];
    formatHMS(sw.elapsedMs(now) / 1000ULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1440:00:12", buf);

    // Milestones stay exact at those magnitudes too (24h interval)
    sw.pollMilestoneMinutes(base, 1440);
    TEST_ASSERT_EQUAL_UINT32(0, sw.pollMilestoneMinutes(base + 24 * HOUR_MS - 1, 1440));
    TEST_ASSERT_EQUAL_UINT32(1440, sw.pollMilestoneMinutes(base + 24 * HOUR_MS, 1440));

    Countdown cd;
    cd.setDurationMs((int64_t)COUNTDOWN_MAX_MS); // 99:59:59
    cd.start(base);
    TEST_ASSERT_EQUAL_UINT64(COUNTDOWN_MAX_MS, cd.remainingMs(base));
    TEST_ASSERT_FALSE(cd.pollFinished(base + COUNTDOWN_MAX_MS - 1));
    TEST_ASSERT_TRUE(cd.pollFinished(base + COUNTDOWN_MAX_MS));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_stopwatch_start_and_elapsed);
    RUN_TEST(test_stopwatch_pause_and_resume);
    RUN_TEST(test_stopwatch_reset);
    RUN_TEST(test_stopwatch_beyond_24_hours);
    RUN_TEST(test_countdown_start_and_remaining);
    RUN_TEST(test_countdown_pause_and_resume);
    RUN_TEST(test_countdown_reaches_exactly_zero);
    RUN_TEST(test_countdown_never_negative);
    RUN_TEST(test_countdown_reset_restores_duration);
    RUN_TEST(test_countdown_duration_clamping);
    RUN_TEST(test_stopwatch_milestone_detection);
    RUN_TEST(test_countdown_milestone_detection);
    RUN_TEST(test_no_duplicate_milestones);
    RUN_TEST(test_milestone_crossed_by_late_update);
    RUN_TEST(test_multiple_milestones_in_one_update);
    RUN_TEST(test_countdown_non_multiple_duration);
    RUN_TEST(test_invalid_interval_disables_milestones);
    RUN_TEST(test_timer_formatting);
    RUN_TEST(test_hidden_seconds_display);
    RUN_TEST(test_64bit_timestamps_long_running);
    return UNITY_END();
}
