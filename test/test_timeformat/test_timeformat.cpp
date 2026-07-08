// Host unit tests for puretime::formatHHMM (timeFormat.cpp) - the 12/24-hour
// clock rendering split out of ClockLogic's formatHHMM.

#include <unity.h>

#include "timeFormat.h"

void setUp() {}
void tearDown() {}

void test_24h_basic()
{
    bool pm;
    TEST_ASSERT_EQUAL_STRING("00:00", puretime::formatHHMM(0, 0, true, pm).c_str());
    TEST_ASSERT_EQUAL_STRING("09:05", puretime::formatHHMM(9, 5, true, pm).c_str());
    TEST_ASSERT_EQUAL_STRING("23:59", puretime::formatHHMM(23, 59, true, pm).c_str());
}

void test_12h_midnight_and_noon_show_twelve()
{
    bool pm;
    TEST_ASSERT_EQUAL_STRING("12:00", puretime::formatHHMM(0, 0, false, pm).c_str());  // midnight
    TEST_ASSERT_FALSE(pm);
    TEST_ASSERT_EQUAL_STRING("12:00", puretime::formatHHMM(12, 0, false, pm).c_str()); // noon
    TEST_ASSERT_TRUE(pm);
}

void test_12h_afternoon_conversion()
{
    bool pm;
    TEST_ASSERT_EQUAL_STRING("01:30", puretime::formatHHMM(13, 30, false, pm).c_str());
    TEST_ASSERT_TRUE(pm);
    TEST_ASSERT_EQUAL_STRING("11:45", puretime::formatHHMM(23, 45, false, pm).c_str());
    TEST_ASSERT_TRUE(pm);
}

void test_pm_flag_independent_of_mode()
{
    bool pm;
    puretime::formatHHMM(11, 0, true, pm);
    TEST_ASSERT_FALSE(pm); // 11:00 is AM
    puretime::formatHHMM(12, 0, true, pm);
    TEST_ASSERT_TRUE(pm);  // noon counts as pm
}

void test_out_of_range_hour_does_not_overflow()
{
    // ezTime can hand a wrapped value (e.g. 249) before the first sync; it must
    // render without smashing the buffer, not crash.
    bool pm;
    std::string s = puretime::formatHHMM(249, 199, true, pm);
    TEST_ASSERT_TRUE(s.length() >= 5);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_24h_basic);
    RUN_TEST(test_12h_midnight_and_noon_show_twelve);
    RUN_TEST(test_12h_afternoon_conversion);
    RUN_TEST(test_pm_flag_independent_of_mode);
    RUN_TEST(test_out_of_range_hour_does_not_overflow);
    return UNITY_END();
}
