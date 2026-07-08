// Host unit tests for orderWifiCandidates - the credential-ordering decision
// behind the "stuck at System initializing..." bug (a phone-portal-saved
// network was never retried after reboot). See wifiCredentials.cpp.

#include <unity.h>
#include <cstring>

#include "wifiCredentials.h"

void setUp() {}
void tearDown() {}

// --- The regression: portal-saved network must be tried, and tried FIRST ----

void test_saved_and_builtin_saved_comes_first()
{
    WifiCandidate out[2];
    int n = orderWifiCandidates("HomeNet", "homepass", "OfficeNet", "officepass", out, 2);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("HomeNet", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("homepass", out[0].pass);
    TEST_ASSERT_EQUAL_STRING("saved", out[0].source);
    TEST_ASSERT_EQUAL_STRING("OfficeNet", out[1].ssid);
    TEST_ASSERT_EQUAL_STRING("built-in", out[1].source);
}

void test_saved_only_when_builtin_empty()
{
    // The exact bug scenario: user configured WiFi via phone, secrets.h left
    // as the empty placeholder. The saved network must still be tried.
    WifiCandidate out[2];
    int n = orderWifiCandidates("HomeNet", "homepass", "", "", out, 2);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("HomeNet", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("saved", out[0].source);
}

void test_builtin_only_when_no_saved()
{
    // First boot ever, before the portal has saved anything.
    WifiCandidate out[2];
    int n = orderWifiCandidates("", "", "OfficeNet", "officepass", out, 2);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("OfficeNet", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("built-in", out[0].source);
}

void test_none_when_both_empty()
{
    WifiCandidate out[2];
    int n = orderWifiCandidates("", "", "", "", out, 2);
    TEST_ASSERT_EQUAL_INT(0, n);
}

void test_null_arguments_are_safe()
{
    WifiCandidate out[2];
    int n = orderWifiCandidates(nullptr, nullptr, nullptr, nullptr, out, 2);
    TEST_ASSERT_EQUAL_INT(0, n);
}

// --- De-duplication ---------------------------------------------------------

void test_identical_saved_and_builtin_deduped()
{
    // Saved network IS the secrets.h network: don't queue it twice.
    WifiCandidate out[2];
    int n = orderWifiCandidates("HomeNet", "homepass", "HomeNet", "homepass", out, 2);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("HomeNet", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("saved", out[0].source);
}

void test_same_ssid_different_pass_kept_separate()
{
    // Same SSID but a different password (e.g. the portal fixed a typo) is a
    // genuinely different credential - keep both.
    WifiCandidate out[2];
    int n = orderWifiCandidates("HomeNet", "newpass", "HomeNet", "oldpass", out, 2);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("newpass", out[0].pass);
    TEST_ASSERT_EQUAL_STRING("oldpass", out[1].pass);
}

// --- Capacity & truncation --------------------------------------------------

void test_open_network_empty_password()
{
    WifiCandidate out[2];
    int n = orderWifiCandidates("OpenCafe", "", "", "", out, 2);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("OpenCafe", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("", out[0].pass);
}

void test_capacity_one_keeps_saved()
{
    WifiCandidate out[1];
    int n = orderWifiCandidates("HomeNet", "homepass", "OfficeNet", "officepass", out, 1);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("HomeNet", out[0].ssid);
}

void test_overlong_inputs_truncated_and_terminated()
{
    char longSsid[80];
    memset(longSsid, 'A', sizeof(longSsid));
    longSsid[sizeof(longSsid) - 1] = '\0'; // 78 'A's
    WifiCandidate out[2];
    int n = orderWifiCandidates(longSsid, "p", "", "", out, 2);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(32, (int)strlen(out[0].ssid)); // capped to 32 chars + NUL
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_saved_and_builtin_saved_comes_first);
    RUN_TEST(test_saved_only_when_builtin_empty);
    RUN_TEST(test_builtin_only_when_no_saved);
    RUN_TEST(test_none_when_both_empty);
    RUN_TEST(test_null_arguments_are_safe);
    RUN_TEST(test_identical_saved_and_builtin_deduped);
    RUN_TEST(test_same_ssid_different_pass_kept_separate);
    RUN_TEST(test_open_network_empty_password);
    RUN_TEST(test_capacity_one_keeps_saved);
    RUN_TEST(test_overlong_inputs_truncated_and_terminated);
    return UNITY_END();
}
