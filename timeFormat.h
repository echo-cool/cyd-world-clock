#ifndef TIME_FORMAT_H
#define TIME_FORMAT_H

#include <string>

// ---------------------------------------------------------------------------
// Clock-face time formatting, split out of ClockLogic's formatHHMM so the
// 12/24-hour conversion is unit-tested on the host (test/test_timeformat)
// without pulling in ezTime. Pure: takes an already-extracted 24-hour value.
// ---------------------------------------------------------------------------

namespace puretime
{
// Render "HH:MM" (zero-padded). pm is set to whether the 24-hour hour is in
// the afternoon (hour24 >= 12), for the AM/PM indicator drawn in 12-hour mode.
// When !show24 the hour is mapped to 1-12 (0 and 12 both shown as 12).
// hour24 is expected in 0-23 but out-of-range values are formatted as-is
// (%02d, up to 3 digits) rather than crashing - mirrors the firmware's
// defence against ezTime returning a wrapped value before the first sync.
std::string formatHHMM(int hour24, int minute, bool show24, bool &pm);
} // namespace puretime

// Three-letter uppercase weekday names indexed by the Arduino/ezTime
// weekday() convention (1 = Sunday); index 0 is unused. One shared copy for
// every face renderer.
constexpr const char *DAY_NAMES[8] = {"",    "SUN", "MON", "TUE",
                                      "WED", "THU", "FRI", "SAT"};

#endif // TIME_FORMAT_H
