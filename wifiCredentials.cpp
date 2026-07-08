#include "wifiCredentials.h"

#include <cstring>

static bool isEmpty(const char *s) { return s == nullptr || s[0] == '\0'; }

// Copy up to sizeof(dst)-1 chars and always NUL-terminate.
static void copyField(char *dst, size_t dstSize, const char *src)
{
    if (dstSize == 0) return;
    if (src == nullptr) src = "";
    size_t n = 0;
    while (src[n] != '\0' && n < dstSize - 1)
    {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

int orderWifiCandidates(const char *savedSsid, const char *savedPass,
                        const char *builtinSsid, const char *builtinPass,
                        WifiCandidate *out, int outCap)
{
    int count = 0;

    // Portal-saved network first: the user typed it into the captive portal on
    // purpose.
    if (!isEmpty(savedSsid) && count < outCap)
    {
        copyField(out[count].ssid, sizeof(out[count].ssid), savedSsid);
        copyField(out[count].pass, sizeof(out[count].pass), savedPass);
        out[count].source = "saved";
        count++;
    }

    // Compiled-in secrets.h pair, unless it is the exact same network already
    // queued as "saved" (matching the field-truncated values, so an over-long
    // input can't sneak past the dedupe).
    if (!isEmpty(builtinSsid) && count < outCap)
    {
        WifiCandidate builtin;
        copyField(builtin.ssid, sizeof(builtin.ssid), builtinSsid);
        copyField(builtin.pass, sizeof(builtin.pass), builtinPass);
        builtin.source = "built-in";

        bool duplicate = (count > 0 &&
                          strcmp(out[0].ssid, builtin.ssid) == 0 &&
                          strcmp(out[0].pass, builtin.pass) == 0);
        if (!duplicate)
        {
            out[count] = builtin;
            count++;
        }
    }

    return count;
}
