#include "timeFormat.h"

#include <cstdio>

namespace puretime
{
std::string formatHHMM(int hour24, int minute, bool show24, bool &pm)
{
    int hr = hour24;
    pm = (hr >= 12);
    if (!show24)
    {
        hr = hr % 12;
        if (hr == 0) hr = 12; // midnight / noon shown as 12, not 0
    }
    // Sized for out-of-range values too: ezTime's hour()/minute() return a
    // uint8_t, and on a negative pre-sync time_t that can be a 3-digit
    // wraparound (e.g. 249) - it must render as garbage, not smash the stack.
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", hr, minute);
    return std::string(buf);
}
} // namespace puretime
