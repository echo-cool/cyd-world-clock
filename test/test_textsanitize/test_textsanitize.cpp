// Host unit tests for puretext::sanitizeHostname / parseMac / normalizeMac
// (textSanitize.cpp), the cores behind projectConfig::sanitizeHostname and
// netCheck::parseMac / normalizeMac.

#include <unity.h>
#include <cstdint>

#include "textSanitize.h"

void setUp() {}
void tearDown() {}

// --- sanitizeHostname -------------------------------------------------------

void test_hostname_lowercases_and_filters()
{
    // Lowercased; dashes kept; everything outside [a-z0-9-] (here '_' and '!')
    // is dropped, NOT converted to a dash.
    TEST_ASSERT_EQUAL_STRING("my-clock01",
                             puretext::sanitizeHostname("My-Clock_01!").c_str());
}

void test_hostname_trims_edge_dashes()
{
    TEST_ASSERT_EQUAL_STRING("clock",
                             puretext::sanitizeHostname("---clock---").c_str());
}

void test_hostname_empty_falls_back_to_default()
{
    TEST_ASSERT_EQUAL_STRING("esp32worldclock", puretext::sanitizeHostname("").c_str());
    // A string of only invalid characters is also "empty" after filtering.
    TEST_ASSERT_EQUAL_STRING("esp32worldclock",
                             puretext::sanitizeHostname("___!!!___").c_str());
}

void test_hostname_capped_at_32()
{
    std::string longName(50, 'a');
    std::string out = puretext::sanitizeHostname(longName);
    TEST_ASSERT_EQUAL_INT(32, (int)out.length());
}

void test_hostname_cap_before_dash_trim()
{
    // The 32-char cap is applied while scanning; if char 32+ would have been
    // the only non-dash content, edge-dash trimming still runs on the capped
    // string. Here the first 32 chars are valid, keeping a clean result.
    std::string in = "abcdefghijklmnopqrstuvwxyz012345extra";
    std::string out = puretext::sanitizeHostname(in);
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnopqrstuvwxyz012345", out.c_str());
}

// --- parseMac ---------------------------------------------------------------

void test_parsemac_colon_form()
{
    uint8_t m[6] = {0};
    TEST_ASSERT_TRUE(puretext::parseMac("DE:AD:BE:EF:12:34", m));
    TEST_ASSERT_EQUAL_HEX8(0xDE, m[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAD, m[1]);
    TEST_ASSERT_EQUAL_HEX8(0x34, m[5]);
}

void test_parsemac_dash_form_and_lowercase()
{
    uint8_t m[6] = {0};
    TEST_ASSERT_TRUE(puretext::parseMac("de-ad-be-ef-12-34", m));
    TEST_ASSERT_EQUAL_HEX8(0xDE, m[0]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, m[3]);
}

void test_parsemac_rejects_malformed()
{
    uint8_t m[6] = {0};
    TEST_ASSERT_FALSE(puretext::parseMac("not-a-mac", m));
    TEST_ASSERT_FALSE(puretext::parseMac("DE:AD:BE:EF:12", m));   // too few
    TEST_ASSERT_FALSE(puretext::parseMac("", m));
    TEST_ASSERT_FALSE(puretext::parseMac("DE:AD:BE:EF:12:1FF", m)); // octet > 0xFF
}

// --- normalizeMac -----------------------------------------------------------

void test_normalizemac_canonicalises()
{
    TEST_ASSERT_EQUAL_STRING("DE:AD:BE:EF:12:34",
                             puretext::normalizeMac("de-ad-be-ef-12-34").c_str());
    TEST_ASSERT_EQUAL_STRING("DE:AD:BE:EF:12:34",
                             puretext::normalizeMac("  DE:AD:BE:EF:12:34  ").c_str());
}

void test_normalizemac_blank_is_empty()
{
    TEST_ASSERT_EQUAL_STRING("", puretext::normalizeMac("").c_str());
    TEST_ASSERT_EQUAL_STRING("", puretext::normalizeMac("   ").c_str());
}

void test_normalizemac_keeps_typo_visible()
{
    // Non-blank but unparseable: returned trimmed-but-unchanged so the user
    // can see and fix it, rather than silently vanishing.
    TEST_ASSERT_EQUAL_STRING("oops", puretext::normalizeMac("  oops  ").c_str());
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_hostname_lowercases_and_filters);
    RUN_TEST(test_hostname_trims_edge_dashes);
    RUN_TEST(test_hostname_empty_falls_back_to_default);
    RUN_TEST(test_hostname_capped_at_32);
    RUN_TEST(test_hostname_cap_before_dash_trim);
    RUN_TEST(test_parsemac_colon_form);
    RUN_TEST(test_parsemac_dash_form_and_lowercase);
    RUN_TEST(test_parsemac_rejects_malformed);
    RUN_TEST(test_normalizemac_canonicalises);
    RUN_TEST(test_normalizemac_blank_is_empty);
    RUN_TEST(test_normalizemac_keeps_typo_visible);
    return UNITY_END();
}
