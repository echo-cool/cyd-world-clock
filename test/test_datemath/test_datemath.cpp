// Host unit tests for daysFromCivil / civilFromDays (dateMath.cpp).

#include <unity.h>

#include "dateMath.h"

void setUp() {}
void tearDown() {}

// Day of week from the epoch-day count: ((days + 4) % 7), 0 = Sunday.
static int dow(int y, int m, int d)
{
    long days = daysFromCivil(y, m, d);
    int wd = (int)((days % 7 + 7 + 4) % 7); // guard against negative days
    return wd; // 0=Sun .. 6=Sat
}

void test_epoch_is_zero()
{
    TEST_ASSERT_EQUAL_INT32(0, daysFromCivil(1970, 1, 1));
}

void test_known_offsets()
{
    TEST_ASSERT_EQUAL_INT32(1, daysFromCivil(1970, 1, 2));
    TEST_ASSERT_EQUAL_INT32(31, daysFromCivil(1970, 2, 1));
    TEST_ASSERT_EQUAL_INT32(365, daysFromCivil(1971, 1, 1)); // 1970 not a leap year
    // 2000-01-01 is 30 years of days after the epoch.
    TEST_ASSERT_EQUAL_INT32(10957, daysFromCivil(2000, 1, 1));
}

void test_days_before_epoch_are_negative()
{
    TEST_ASSERT_EQUAL_INT32(-1, daysFromCivil(1969, 12, 31));
    TEST_ASSERT_EQUAL_INT32(-365, daysFromCivil(1969, 1, 1));
}

void test_day_of_week_known_dates()
{
    // 1970-01-01 was a Thursday (4).
    TEST_ASSERT_EQUAL_INT(4, dow(1970, 1, 1));
    // 2000-01-01 was a Saturday (6).
    TEST_ASSERT_EQUAL_INT(6, dow(2000, 1, 1));
    // 2026-07-07 is a Tuesday (2).
    TEST_ASSERT_EQUAL_INT(2, dow(2026, 7, 7));
}

void test_leap_day_and_boundary()
{
    // 2000 is a leap year (divisible by 400): Feb 29 exists and Mar 1 follows.
    long feb29 = daysFromCivil(2000, 2, 29);
    long mar1 = daysFromCivil(2000, 3, 1);
    TEST_ASSERT_EQUAL_INT32(1, (int)(mar1 - feb29));
    // 1900 is NOT a leap year (divisible by 100 but not 400).
    long y1900_feb28 = daysFromCivil(1900, 2, 28);
    long y1900_mar1 = daysFromCivil(1900, 3, 1);
    TEST_ASSERT_EQUAL_INT32(1, (int)(y1900_mar1 - y1900_feb28));
}

void test_roundtrip_over_a_century()
{
    // civilFromDays must invert daysFromCivil for every day across 1970-2069.
    for (long day = daysFromCivil(1970, 1, 1); day <= daysFromCivil(2069, 12, 31); day++)
    {
        int y, m, d;
        civilFromDays(day, y, m, d);
        TEST_ASSERT_EQUAL_INT32(day, daysFromCivil(y, m, d));
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_epoch_is_zero);
    RUN_TEST(test_known_offsets);
    RUN_TEST(test_days_before_epoch_are_negative);
    RUN_TEST(test_day_of_week_known_dates);
    RUN_TEST(test_leap_day_and_boundary);
    RUN_TEST(test_roundtrip_over_a_century);
    return UNITY_END();
}
