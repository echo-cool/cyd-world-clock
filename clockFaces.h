#ifndef CLOCK_FACES_H
#define CLOCK_FACES_H

// ---------------------------------------------------------------------------
// Alternate home-screen faces. FACE_QUAD is the classic four-quadrant world
// clock rendered by ClockLogic.cpp; the other faces are rendered here. The
// active face is projectConfig.clockFace, cycled from the settings page or by
// tapping the lower-left / lower-right corners of the home screen.
//
// All faces share the home-screen touch zones (center = settings, lower-left/
// lower-right corners = previous/next face, rest of the left/right thirds =
// brightness).
// ---------------------------------------------------------------------------

#include <Arduino.h>

enum ClockFace
{
    FACE_QUAD = 0,     // four-quadrant world clock (ClockLogic.cpp)
    FACE_BIG = 1,      // one large home clock + mini strip of the other zones
    FACE_CALENDAR = 2, // month calendar for the home zone + clock
    FACE_WEATHER = 3,  // current weather for all four cities + clock
    FACE_MARKETS = 4,  // all known stock exchanges with status/countdown
    FACE_COUNT = 5
};

// Short label for the settings-page button.
const char *clockFaceName(int face);

// Render the selected alternate face. Called from drawRollingClock when the
// home screen is showing and projectConfig.clockFace != FACE_QUAD. Repaints
// fully when firstDraw is set (or the face just changed) and refreshes the
// dynamic parts once per minute (the weather face also repaints as soon as
// the background fetch task delivers fresh data); does nothing while the
// brightness bar overlay is visible.
void drawAlternateFace();

#endif // CLOCK_FACES_H
