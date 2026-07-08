// Host unit tests for market:: session-timing math (marketSession.cpp), the
// midnight-spanning logic extracted from getMarketStatus. Times are minutes
// from local midnight.

#include <unity.h>

#include "marketSession.h"

void setUp() {}
void tearDown() {}

// NYSE regular session 09:30-16:00.
static const int REG_OPEN = 9 * 60 + 30;  // 570
static const int REG_CLOSE = 16 * 60;     // 960
// Overnight future 20:00-04:00 (spans midnight).
static const int ON_OPEN = 20 * 60;       // 1200
static const int ON_CLOSE = 4 * 60;       // 240

// --- Normal, same-day session ----------------------------------------------

void test_normal_in_session()
{
    TEST_ASSERT_TRUE(market::inSession(10 * 60, REG_OPEN, REG_CLOSE));   // 10:00
    TEST_ASSERT_TRUE(market::inSession(REG_OPEN, REG_OPEN, REG_CLOSE));  // open is inclusive
}

void test_normal_out_of_session_boundaries()
{
    TEST_ASSERT_FALSE(market::inSession(REG_OPEN - 1, REG_OPEN, REG_CLOSE)); // 09:29
    TEST_ASSERT_FALSE(market::inSession(REG_CLOSE, REG_OPEN, REG_CLOSE));    // close is exclusive
    TEST_ASSERT_FALSE(market::inSession(20 * 60, REG_OPEN, REG_CLOSE));      // evening
}

void test_normal_minutes_until_close()
{
    TEST_ASSERT_EQUAL_INT(360, market::minutesUntilClose(10 * 60, REG_OPEN, REG_CLOSE)); // 10:00 -> 16:00
    TEST_ASSERT_EQUAL_INT(1, market::minutesUntilClose(REG_CLOSE - 1, REG_OPEN, REG_CLOSE));
}

void test_normal_minutes_until_open()
{
    TEST_ASSERT_EQUAL_INT(5, market::minutesUntilOpen(REG_OPEN - 5, REG_OPEN, REG_CLOSE)); // 09:25
    // Already opened/closed today -> negative (caller looks to a later day).
    TEST_ASSERT_TRUE(market::minutesUntilOpen(10 * 60, REG_OPEN, REG_CLOSE) < 0);
    TEST_ASSERT_TRUE(market::minutesUntilOpen(REG_CLOSE + 60, REG_OPEN, REG_CLOSE) < 0);
}

// --- Midnight-spanning session ---------------------------------------------

void test_spanning_in_session_both_halves()
{
    TEST_ASSERT_TRUE(market::inSession(21 * 60, ON_OPEN, ON_CLOSE));  // 21:00 (pre-midnight)
    TEST_ASSERT_TRUE(market::inSession(2 * 60, ON_OPEN, ON_CLOSE));   // 02:00 (post-midnight)
    TEST_ASSERT_TRUE(market::inSession(0, ON_OPEN, ON_CLOSE));        // exactly midnight
    TEST_ASSERT_TRUE(market::inSession(ON_OPEN, ON_OPEN, ON_CLOSE));  // open inclusive
}

void test_spanning_out_of_session()
{
    TEST_ASSERT_FALSE(market::inSession(10 * 60, ON_OPEN, ON_CLOSE)); // 10:00 daytime gap
    TEST_ASSERT_FALSE(market::inSession(ON_CLOSE, ON_OPEN, ON_CLOSE)); // 04:00 close exclusive
    TEST_ASSERT_FALSE(market::inSession(ON_OPEN - 1, ON_OPEN, ON_CLOSE)); // 19:59
}

void test_spanning_minutes_until_close()
{
    // 21:40 -> 04:00 = 6h20m = 380 (crosses midnight).
    TEST_ASSERT_EQUAL_INT(380, market::minutesUntilClose(21 * 60 + 40, ON_OPEN, ON_CLOSE));
    // 02:00 -> 04:00 = 120 (already past midnight).
    TEST_ASSERT_EQUAL_INT(120, market::minutesUntilClose(2 * 60, ON_OPEN, ON_CLOSE));
}

void test_spanning_minutes_until_open()
{
    // 10:00 -> 20:00 = 600.
    TEST_ASSERT_EQUAL_INT(600, market::minutesUntilOpen(10 * 60, ON_OPEN, ON_CLOSE));
    // 04:00 -> 20:00 = 960 (after the close, before the next open).
    TEST_ASSERT_EQUAL_INT(960, market::minutesUntilOpen(ON_CLOSE, ON_OPEN, ON_CLOSE));
    // Late evening after open wraps to the next day's open.
    TEST_ASSERT_EQUAL_INT(1440 - (21 * 60) + ON_OPEN,
                          market::minutesUntilOpen(21 * 60, ON_OPEN, ON_CLOSE));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_normal_in_session);
    RUN_TEST(test_normal_out_of_session_boundaries);
    RUN_TEST(test_normal_minutes_until_close);
    RUN_TEST(test_normal_minutes_until_open);
    RUN_TEST(test_spanning_in_session_both_halves);
    RUN_TEST(test_spanning_out_of_session);
    RUN_TEST(test_spanning_minutes_until_close);
    RUN_TEST(test_spanning_minutes_until_open);
    return UNITY_END();
}
