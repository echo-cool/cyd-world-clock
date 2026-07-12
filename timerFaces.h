#ifndef TIMER_FACES_H
#define TIMER_FACES_H

// ---------------------------------------------------------------------------
// Device integration for the stopwatch / countdown clock faces: rendering,
// touch buttons, the milestone reminder banner and the final countdown alarm.
// The timer state machines themselves are hardware-independent and live in
// timerLogic.h (host-tested); this module feeds them the monotonic
// esp_timer clock and draws the results.
//
// Unlike the passive faces, the timer faces draw visible buttons instead of
// the home screen's invisible touch zones:
//   [-30][-5][+5][+30]  duration row (countdown, only while stopped)
//   [START/PAUSE/RESUME/RESTART]  [RESET]
//   [<]      [SETTINGS]      [>]
// Face cycling ([<] / [>]) and Settings stay available; brightness is
// adjusted from the settings page (or the web UI) while a timer face is
// showing.
// ---------------------------------------------------------------------------

#include <Arduino.h>

// Advance both timer engines and detect milestone / completion events. Must
// run every loop iteration regardless of the active face or UI page, so the
// timers keep counting (and the alarm still fires) while other screens are
// open. MAIN core only.
void timersService();

// Final countdown alarm: while active it owns the display (flashing red/white
// "TIME'S UP" every ~500ms) and the touch panel (any tap acknowledges it; the
// tap is consumed). Returns true while the alarm owns the screen - callers
// skip all other UI work. After acknowledgement the UI always switches to the
// home screen showing the countdown face in its FINISHED state.
bool timerAlarmService();

// Milestone reminder banner: a ~1.6s non-blocking strip across the top of
// whatever screen is currently showing ("30 MINUTES ELAPSED" / "... REMAINING"),
// then a full repaint of that screen. Deferred while the brightness bar,
// touch calibration screen or an OTA update owns the display.
void timerOverlayService();

// Face renderers, called from drawAlternateFace every loop while the face is
// active; they repaint only the regions whose content changed (the second
// counter, state changes), full repaints only when `full` is set.
void renderStopwatchFace(bool full);
void renderCountdownFace(bool full);

// Route a home-screen touch (already in screen pixels) to the visible timer
// buttons while a timer face is showing. Recognized taps consume the touch
// (suppressed until release); taps outside every button are ignored.
void timerFaceHandleTouch(int x, int y);

// Re-apply projectConfig.countdownDefaultMin to the countdown session if it
// is not running (web settings changed the default).
void timersApplyConfigDefaults();

// Timer state for /api/status.
const char *stopwatchStateName();
uint32_t stopwatchElapsedSec();
const char *countdownStateName();
uint32_t countdownConfiguredSec();
uint32_t countdownRemainingSec();

#endif // TIMER_FACES_H
