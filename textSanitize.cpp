#include "textSanitize.h"

#include <cstdio>

namespace puretext
{
std::string sanitizeHostname(const std::string &raw)
{
    std::string out;
    for (size_t i = 0; i < raw.length() && out.length() < 32; i++)
    {
        char c = raw[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
        {
            out += c;
        }
    }
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "esp32worldclock";
    return out;
}

bool parseMac(const std::string &s, uint8_t out[6])
{
    unsigned int v[6];
    int n = sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6)
    {
        n = sscanf(s.c_str(), "%x-%x-%x-%x-%x-%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    }
    if (n != 6)
        return false;
    for (int i = 0; i < 6; i++)
    {
        if (v[i] > 0xFF)
            return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

// Trim ASCII whitespace from both ends (matches Arduino String::trim()).
static std::string trimmed(const std::string &s)
{
    size_t b = 0, e = s.length();
    while (b < e && (unsigned char)s[b] <= ' ') b++;
    while (e > b && (unsigned char)s[e - 1] <= ' ') e--;
    return s.substr(b, e - b);
}

std::string normalizeMac(const std::string &s)
{
    std::string t = trimmed(s);
    if (t.empty())
        return "";
    uint8_t m[6];
    if (!parseMac(t, m))
        return t; // keep the typo visible so the user can fix it
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return std::string(buf);
}
} // namespace puretext
