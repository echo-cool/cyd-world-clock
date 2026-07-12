#ifndef CLOCK_FACES_H
#define CLOCK_FACES_H

// ---------------------------------------------------------------------------
// Alternate home-screen faces. FACE_QUAD is the classic four-quadrant world
// clock rendered by ClockLogic.cpp; the timer faces are rendered by
// timerFaces.cpp and the other faces here. The active face is
// projectConfig.clockFace, cycled from the settings page or by tapping the
// lower-left / lower-right corners of the home screen.
//
// The passive faces share the home-screen touch zones (center = settings,
// lower-left/lower-right corners = previous/next face, rest of the left/right
// thirds = brightness). The stopwatch/countdown faces draw visible buttons
// instead ([<] / [SETTINGS] / [>] plus the timer controls - see timerFaces.h);
// brightness is adjusted from the settings page while they are showing.
// ---------------------------------------------------------------------------

#include <Arduino.h>

enum ClockFace
{
    FACE_QUAD = 0,      // four-quadrant world clock (ClockLogic.cpp)
    FACE_BIG = 1,       // one large home clock + mini strip of the other zones
    FACE_CALENDAR = 2,  // month calendar for the home zone + clock
    FACE_WEATHER = 3,   // current weather for all four cities + clock
    FACE_MARKETS = 4,   // all known stock exchanges with status/countdown
    FACE_STOPWATCH = 5, // count-up timer with milestone reminders (timerFaces.cpp)
    FACE_COUNTDOWN = 6, // countdown timer with final alarm (timerFaces.cpp)
    FACE_COUNT = 7
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
