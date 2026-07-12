#include "timerFaces.h"

#include <esp_timer.h> // 64-bit monotonic clock - immune to NTP steps

#include "ClockLogic.h" // tft, worldZones, readTouchPoint, formatHHMM
#include "clockFaces.h" // ClockFace enum, clockFaceName
#include "otaUpdate.h"  // otaInProgress - defer the banner during updates
#include "projectConfig.h"
#include "timerLogic.h"
#include "uiPages.h" // uiScreen, switchToScreen, touchSuppressedUntilRelease

// renderUiPage's "page needs a full repaint" flag (uiPages.cpp); the banner
// cleanup uses it to restore whatever settings/status page it painted over.
extern bool uiPageDrawn;

static timerlogic::Stopwatch stopwatch;
static timerlogic::Countdown countdown;
static bool timersInited = false;

static const char *DAY_NAMES[8] = {"", "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// Monotonic milliseconds since boot, 64-bit (esp_timer_get_time is us). Never
// wraps and never jumps when NTP adjusts the wall clock.
static uint64_t monoMs()
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

/*-------- Screen scaling ----------*/
// Same 320x240 design-coordinate mapping as the other alternate faces
// (clockFaces.cpp sx/sy) and the touch UI's scaleUiX/Y.

static int tfx(int v)
{
    return (v * screenWidth + 160) / 320;
}

static int tfy(int v)
{
    return (v * screenHeight + 120) / 240;
}

static bool largeTimerFace()
{
    return screenWidth >= 440 && screenHeight >= 300;
}

/*-------- Buttons ----------*/
// Visible controls replace the home screen's invisible touch zones on the
// timer faces. Same rects are used for drawing and hit-testing, so drawn
// buttons and touch targets can never drift apart.

struct TimerBtn
{
    int x, y, w, h; // design coordinates (320x240)
};

// Duration row (countdown only, while stopped)
static const TimerBtn BTN_ADJ[4] = {
    {8, 114, 72, 30}, {86, 114, 72, 30}, {164, 114, 72, 30}, {242, 114, 70, 30}};
static const char *ADJ_LABELS[4] = {"-30 MIN", "-5 MIN", "+5 MIN", "+30 MIN"};
static const int64_t ADJ_DELTA_MS[4] = {-30LL * 60000LL, -5LL * 60000LL,
                                        5LL * 60000LL, 30LL * 60000LL};

// Primary action (START/PAUSE/RESUME/RESTART) and the deliberately separated
// RESET, so an off-target tap on the primary button cannot wipe the timer.
static const TimerBtn BTN_PRIMARY = {24, 152, 148, 36};
static const TimerBtn BTN_RESET = {200, 152, 96, 36};

// Bottom row: previous face / settings / focus toggle / next face. The first
// three are the global actions the other faces put on invisible corner and
// center zones; HIDE SEC / SHOW SEC switches the big timer to a calm HH:MM
// that only changes once a minute (persisted: projectConfig.timerHideSeconds).
static const TimerBtn BTN_PREV = {6, 198, 42, 38};
static const TimerBtn BTN_SET = {54, 198, 104, 38};
static const TimerBtn BTN_FOCUS = {164, 198, 104, 38};
static const TimerBtn BTN_NEXT = {274, 198, 40, 38};

static bool timerBtnHit(const TimerBtn &b, int tx, int ty)
{
    int x = tfx(b.x), y = tfy(b.y), w = tfx(b.w), h = tfy(b.h);
    return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

static void drawTimerBtn(const TimerBtn &b, const char *label, uint16_t border,
                         uint16_t textColor)
{
    int x = tfx(b.x), y = tfy(b.y), w = tfx(b.w), h = tfy(b.h);
    tft.fillRect(x, y, w, h, clockBackgroundColor); // erase the previous label
    tft.drawRoundRect(x, y, w, h, 6, border);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(textColor, clockBackgroundColor);
    tft.drawString(label, x + w / 2, y + h / 2);
}

/*-------- Milestone reminder banner ----------*/
// Non-blocking: queued by timersService, painted (and later cleaned up via a
// full repaint) by timerOverlayService from the normal draw loop.

static char overlayText[48] = "";
static unsigned long overlayUntilMs = 0; // millis() deadline; 0 = idle
static unsigned long overlayLastDrawMs = 0;
static bool overlayShown = false; // drawn at least once - needs cleanup

/*-------- Final countdown alarm ----------*/

static bool alarmActive = false;
static bool alarmPhase = false; // false = red/white, true = white/red
static unsigned long alarmLastFlipMs = 0;
static bool alarmDrawn = false;

static void queueMilestoneBanner(uint32_t minutes, const char *suffix)
{
    char span[20];
    if (minutes < 60) {
        snprintf(span, sizeof(span), "%u MINUTE%s", (unsigned)minutes,
                 minutes == 1 ? "" : "S");
    } else if (minutes % 60 == 0) {
        snprintf(span, sizeof(span), "%u HOUR%s", (unsigned)(minutes / 60),
                 minutes == 60 ? "" : "S");
    } else {
        snprintf(span, sizeof(span), "%uH %02uM", (unsigned)(minutes / 60),
                 (unsigned)(minutes % 60));
    }
    snprintf(overlayText, sizeof(overlayText), "%s %s", span, suffix);
    overlayUntilMs = millis() + 1600;
    overlayLastDrawMs = 0;
    Log.println("Timer reminder: " + String(overlayText));
}

/*-------- Engine service (runs every loop, any screen) ----------*/

void timersService()
{
    if (!timersInited) {
        timersInited = true;
        // Session countdown duration starts from the saved default; on-device
        // +/- adjustments then stay session-only (never written to flash).
        countdown.setDurationMs((int64_t)projectConfig.countdownDefaultMin * 60000LL);
    }

    uint64_t now = monoMs();
    int interval = projectConfig.timerReminderMin;

    if (countdown.pollFinished(now)) {
        Log.println("Countdown finished - raising the alarm until acknowledged");
        alarmActive = true;
        alarmPhase = false;
        alarmDrawn = false;
        // A finger already on the glass must not dismiss the alarm instantly.
        touchSuppressedUntilRelease = true;
        // The final alarm outranks any pending milestone banner.
        overlayUntilMs = 0;
        overlayShown = false;
    }

    uint32_t mark = stopwatch.pollMilestoneMinutes(now, interval);
    if (mark > 0 && !alarmActive) {
        queueMilestoneBanner(mark, "ELAPSED");
    }
    mark = countdown.pollMilestoneMinutes(now, interval);
    if (mark > 0 && !alarmActive) {
        queueMilestoneBanner(mark, "REMAINING");
    }
}

void timersApplyConfigDefaults()
{
    if (!timersInited) return; // first timersService() call picks it up
    if (countdown.phase == timerlogic::TIMER_RUNNING ||
        countdown.phase == timerlogic::TIMER_PAUSED) {
        return; // never yank a live countdown; applies after its reset
    }
    countdown.setDurationMs((int64_t)projectConfig.countdownDefaultMin * 60000LL);
}

/*-------- Final alarm (flash until acknowledged) ----------*/

static void drawAlarmFrame()
{
    uint16_t bg = alarmPhase ? TFT_WHITE : TFT_RED;
    uint16_t fg = alarmPhase ? TFT_RED : TFT_WHITE;
    tft.fillScreen(bg);

    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextSize(2);
    tft.setTextColor(fg, bg);
    tft.drawString("TIME'S UP", screenWidth / 2, tfy(66));

    tft.setTextFont(7);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("00:00:00", screenWidth / 2, tfy(102));

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("TAP TO DISMISS", screenWidth / 2, tfy(190));
}

bool timerAlarmService()
{
    if (!alarmActive) return false;

    TouchPoint t = readTouchPoint();
    bool down = (t.zRaw > 800);
    if (!down) {
        touchSuppressedUntilRelease = false;
    } else if (!touchSuppressedUntilRelease) {
        // Any tap acknowledges the alarm; consume it so it cannot click
        // through onto whatever page is drawn next.
        alarmActive = false;
        Log.println("Countdown alarm acknowledged");
        bool dirty = false;
        if (projectConfig.clockFace != FACE_COUNTDOWN) {
            projectConfig.clockFace = FACE_COUNTDOWN;
            dirty = true;
        }
        if (projectConfig.brightness != backlightLevel) {
            // A brightness gesture may have been cut short by the alarm;
            // fold its pending save in instead of dropping it.
            projectConfig.brightness = backlightLevel;
            dirty = true;
        }
        if (dirty) projectConfig.saveConfigFile();
        brightnessBarVisible = false;
        // Always land on the home screen showing the countdown face in its
        // FINISHED state (documented behavior, predictable from any page).
        switchToScreen(SCREEN_HOME);
        return true;
    }

    unsigned long nowMs = millis();
    if (!alarmDrawn || nowMs - alarmLastFlipMs >= 500) {
        if (alarmDrawn) alarmPhase = !alarmPhase;
        alarmLastFlipMs = nowMs;
        alarmDrawn = true;
        drawAlarmFrame();
    }
    return true;
}

/*-------- Milestone banner overlay ----------*/

void timerOverlayService()
{
    if (overlayUntilMs == 0) return;
    unsigned long nowMs = millis();

    if ((long)(nowMs - overlayUntilMs) >= 0) {
        // Expired: restore whatever screen the banner painted over.
        overlayUntilMs = 0;
        if (overlayShown) {
            overlayShown = false;
            if (uiScreen == SCREEN_HOME) {
                firstDraw = true;
                for (int i = 0; i < 4; i++) {
                    worldZones[i].initialized = false;
                }
            } else {
                uiPageDrawn = false;
            }
        }
        return;
    }

    // Defer (don't drop) while another overlay/process owns the display; the
    // touch calibration screen is never painted over at all.
    if (uiScreen == SCREEN_TOUCH_CAL || otaInProgress || brightnessBarVisible) {
        return;
    }

    // Repaint on a short cadence so the per-second face updates underneath
    // can't chew a corner off the banner while it is up.
    if (!overlayShown || nowMs - overlayLastDrawMs >= 250) {
        overlayLastDrawMs = nowMs;
        overlayShown = true;
        int h = tfy(24);
        tft.fillRect(0, 0, screenWidth, h, TFT_ORANGE);
        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_BLACK, TFT_ORANGE);
        tft.drawString(overlayText, screenWidth / 2, h / 2);
    }
}

/*-------- Face rendering ----------*/
// Called every loop while the face is active (drawAlternateFace routes the
// timer faces around its once-per-minute gating). Only regions whose content
// actually changed are repainted, so the ~50Hz polls cost nothing between
// second ticks and the display never flickers.

static int cachedPhase = -1;
static char cachedBig[16] = "";
static uint16_t cachedBigColor = 0;
static uint8_t cachedBigFont = 0;
static int cachedHeaderMinute = -1;

static void resetRenderCache()
{
    cachedPhase = -1;
    cachedBig[0] = '\0';
    cachedBigColor = 0;
    cachedBigFont = 0;
    cachedHeaderMinute = -1;
}

static uint16_t phaseColor(timerlogic::TimerPhase phase)
{
    switch (phase) {
    case timerlogic::TIMER_RUNNING: return TFT_GREEN;
    case timerlogic::TIMER_PAUSED: return TFT_YELLOW;
    case timerlogic::TIMER_FINISHED: return TFT_RED;
    default: return TFT_LIGHTGREY;
    }
}

// Header: home-zone clock (12/24h preference), weekday + date (date-format
// preference) and the face title. Repainted once per minute.
static void drawTimerHeader(bool full, const char *title)
{
    time_t local = worldZones[0].tz.now();
    if (!full && minute(local) == cachedHeaderMinute) return;
    cachedHeaderMinute = minute(local);

    tft.fillRect(0, 0, screenWidth, tfy(28), clockBackgroundColor);

    bool pm;
    String hhmm = formatHHMM(local, pm);
    if (!SHOW_24HOUR) hhmm += pm ? " PM" : " AM";
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(getDayNightColor(worldZones[0]), clockBackgroundColor);
    tft.drawString(hhmm, tfx(8), 2);

    char dateBuf[8];
    if (NOT_US_DATE) {
        sprintf(dateBuf, "%02d/%02d", day(local), month(local));
    } else {
        sprintf(dateBuf, "%02d/%02d", month(local), day(local));
    }
    tft.setTextFont(2);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString(String(DAY_NAMES[weekday(local)]) + " " + dateBuf,
                   screenWidth - tfx(6), 6);

    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.drawString(title, screenWidth / 2, tfy(10));
}

static void drawStateLine(timerlogic::TimerPhase phase)
{
    bool big = largeTimerFace();
    tft.setTextFont(big ? 4 : 2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(phaseColor(phase), clockBackgroundColor);
    tft.setTextPadding(tfx(200));
    tft.drawString(timerlogic::timerPhaseName(phase), screenWidth / 2, tfy(34));
    tft.setTextPadding(0);
}

// The big HH:MM:SS. Font 7 (7-segment, digits + colon only) at the largest
// size that fits; hour counts past 99 simply widen the string ("125:04:09" -
// never wrapped), falling back to Font 4 in the extreme case where even the
// 7-segment rendering no longer fits the panel.
static void drawBigTime(const char *text, uint16_t color)
{
    int maxW = screenWidth - tfx(8);

    // Font 7 stays at size 1 on both panels: its 48px glyphs are the tallest
    // that fit the band between the state line and the button rows (the band
    // scales to 72px on the 4" panel; size 2 would be 96px and collide).
    tft.setTextFont(7);
    tft.setTextSize(1);
    uint8_t fontKey = 7;
    if (tft.textWidth(text) > maxW) {
        tft.setTextFont(4);
        tft.setTextSize(2);
        fontKey = 4;
    }

    int y = tfy(58);
    if (fontKey != cachedBigFont) {
        // Font or size changed: glyph heights differ, so clear the whole
        // band once instead of relying on padding.
        cachedBigFont = fontKey;
        tft.fillRect(0, y, screenWidth, tfy(112) - y, clockBackgroundColor);
    }

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color, clockBackgroundColor);
    tft.setTextPadding(maxW);
    tft.drawString(text, screenWidth / 2, y);
    tft.setTextPadding(0);
}

static void drawPrimaryButton(timerlogic::TimerPhase phase)
{
    const char *label;
    uint16_t border;
    switch (phase) {
    case timerlogic::TIMER_RUNNING:
        label = "PAUSE";
        border = TFT_YELLOW;
        break;
    case timerlogic::TIMER_PAUSED:
        label = "RESUME";
        border = TFT_GREEN;
        break;
    case timerlogic::TIMER_FINISHED:
        label = "RESTART";
        border = TFT_GREEN;
        break;
    default:
        label = "START";
        border = TFT_GREEN;
        break;
    }
    drawTimerBtn(BTN_PRIMARY, label, border, TFT_WHITE);
}

// Countdown duration row: +/- buttons while stopped, the configured total
// (read-only) while running or paused.
static void drawDurationRow(timerlogic::TimerPhase phase)
{
    tft.fillRect(0, tfy(112), screenWidth, tfy(148) - tfy(112), clockBackgroundColor);
    if (phase == timerlogic::TIMER_RUNNING || phase == timerlogic::TIMER_PAUSED) {
        char buf[16];
        timerlogic::formatHMS(countdown.durationMs / 1000ULL, buf, sizeof(buf));
        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
        tft.drawString("TOTAL " + String(buf), screenWidth / 2, tfy(122));
    } else {
        for (int i = 0; i < 4; i++) {
            drawTimerBtn(BTN_ADJ[i], ADJ_LABELS[i], TFT_CYAN, TFT_WHITE);
        }
    }
}

// Shared frame for both faces; isCountdown selects the engine and the
// duration row.
static void renderTimerFace(bool full, bool isCountdown)
{
    uint64_t now = monoMs();
    timerlogic::TimerPhase phase = isCountdown ? countdown.phase : stopwatch.phase;

    if (full) {
        tft.fillScreen(clockBackgroundColor);
        resetRenderCache();
        tft.drawFastHLine(tfx(10), tfy(30), tfx(300), TFT_DARKGREY);
        drawTimerBtn(BTN_RESET, "RESET", TFT_RED, TFT_WHITE);
        drawTimerBtn(BTN_PREV, "<", TFT_CYAN, TFT_WHITE);
        drawTimerBtn(BTN_SET, "SETTINGS", TFT_CYAN, TFT_WHITE);
        // Green border while focus mode is on, so the calmer display is
        // recognizably a mode and not a stalled clock.
        drawTimerBtn(BTN_FOCUS,
                     projectConfig.timerHideSeconds ? "SHOW SEC" : "HIDE SEC",
                     projectConfig.timerHideSeconds ? TFT_GREEN : TFT_CYAN,
                     TFT_WHITE);
        drawTimerBtn(BTN_NEXT, ">", TFT_CYAN, TFT_WHITE);
        if (!isCountdown && timerlogic::reminderIntervalValid(projectConfig.timerReminderMin)) {
            // The stopwatch has no duration row; use the slot for a reminder
            // hint so the interval is discoverable on the device.
            tft.setTextFont(1);
            tft.setTextSize(1);
            tft.setTextDatum(TC_DATUM);
            tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
            tft.drawString("REMINDER EVERY " + String(projectConfig.timerReminderMin) +
                               " MIN",
                           screenWidth / 2, tfy(124));
        }
    }

    drawTimerHeader(full, isCountdown ? "COUNTDOWN" : "STOPWATCH");

    if (full || (int)phase != cachedPhase) {
        cachedPhase = (int)phase;
        drawStateLine(phase);
        drawPrimaryButton(phase);
        if (isCountdown) drawDurationRow(phase);
    }

    // Focus mode (timerHideSeconds): HH:MM only, so the big display changes
    // just once a minute instead of ticking every second - nothing on the
    // face keeps moving to pull the eye away from work.
    bool hideSec = projectConfig.timerHideSeconds;
    char buf[16];
    uint16_t color;
    if (isCountdown) {
        uint64_t rem = countdown.remainingMs(now);
        if (hideSec) {
            // Ceil to whole minutes: full duration at start, and never 00:00
            // while any time is actually left.
            timerlogic::formatHM(timerlogic::displayMinutesRemaining(rem), buf,
                                 sizeof(buf));
        } else {
            // Ceil to whole seconds: shows the full duration at start and
            // reaches exactly 00:00:00 only when the countdown really is over.
            timerlogic::formatHMS((rem + 999ULL) / 1000ULL, buf, sizeof(buf));
        }
        color = (phase == timerlogic::TIMER_FINISHED)                 ? TFT_RED
                : (phase == timerlogic::TIMER_RUNNING && rem < 60000ULL) ? TFT_ORANGE
                                                                         : TFT_WHITE;
    } else {
        uint64_t elapsed = stopwatch.elapsedMs(now);
        if (hideSec) {
            timerlogic::formatHM(timerlogic::displayMinutesElapsed(elapsed), buf,
                                 sizeof(buf));
        } else {
            timerlogic::formatHMS(elapsed / 1000ULL, buf, sizeof(buf));
        }
        color = TFT_WHITE;
    }
    if (full || color != cachedBigColor || strcmp(buf, cachedBig) != 0) {
        strncpy(cachedBig, buf, sizeof(cachedBig) - 1);
        cachedBig[sizeof(cachedBig) - 1] = '\0';
        cachedBigColor = color;
        drawBigTime(buf, color);
    }
}

void renderStopwatchFace(bool full)
{
    renderTimerFace(full, false);
}

void renderCountdownFace(bool full)
{
    renderTimerFace(full, true);
}

/*-------- Touch ----------*/

// Face cycling from the [<] / [>] buttons - same behavior as the corner taps
// on the passive faces (persist + full repaint + suppress until release).
static void switchFaceBy(int step)
{
    projectConfig.clockFace = (projectConfig.clockFace + step) % FACE_COUNT;
    projectConfig.brightness = backlightLevel; // fold any pending level in
    projectConfig.saveConfigFile();
    Log.println("Timer face button - clock face: " +
                String(clockFaceName(projectConfig.clockFace)));
    tft.fillScreen(clockBackgroundColor);
    firstDraw = true;
    for (int i = 0; i < 4; i++) {
        worldZones[i].initialized = false;
    }
    brightnessBarVisible = false;
    touchSuppressedUntilRelease = true;
}

void timerFaceHandleTouch(int x, int y)
{
    bool isCountdown = (projectConfig.clockFace == FACE_COUNTDOWN);
    uint64_t now = monoMs();
    char buf[16];

    if (timerBtnHit(BTN_SET, x, y)) {
        Log.println("Timer face - opening settings page");
        switchToScreen(SCREEN_SETTINGS); // suppresses touch until release
        return;
    }
    if (timerBtnHit(BTN_PREV, x, y)) {
        switchFaceBy(FACE_COUNT - 1);
        return;
    }
    if (timerBtnHit(BTN_NEXT, x, y)) {
        switchFaceBy(1);
        return;
    }
    if (timerBtnHit(BTN_FOCUS, x, y)) {
        // Toggle the calm HH:MM display. Persisted (a deliberate, discrete
        // tap - fine for flash), applied to both timer faces at once.
        projectConfig.timerHideSeconds = !projectConfig.timerHideSeconds;
        projectConfig.saveConfigFile();
        Log.println(projectConfig.timerHideSeconds
                        ? "Timer faces: seconds hidden (focus mode)"
                        : "Timer faces: seconds shown");
        firstDraw = true; // repaint the face with the new display + button label
        touchSuppressedUntilRelease = true;
        return;
    }

    if (timerBtnHit(BTN_PRIMARY, x, y)) {
        touchSuppressedUntilRelease = true;
        if (isCountdown) {
            if (countdown.start(now)) {
                timerlogic::formatHMS(countdown.durationMs / 1000ULL, buf, sizeof(buf));
                Log.println("Countdown started (" + String(buf) + ")");
            } else if (countdown.pause(now)) {
                Log.println("Countdown paused");
            } else if (countdown.resume(now)) {
                Log.println("Countdown resumed");
            }
        } else {
            if (stopwatch.start(now)) {
                Log.println("Stopwatch started");
            } else if (stopwatch.pause(now)) {
                timerlogic::formatHMS(stopwatch.elapsedMs(now) / 1000ULL, buf, sizeof(buf));
                Log.println("Stopwatch paused at " + String(buf));
            } else if (stopwatch.resume(now)) {
                Log.println("Stopwatch resumed");
            }
        }
        return;
    }

    if (timerBtnHit(BTN_RESET, x, y)) {
        touchSuppressedUntilRelease = true;
        if (isCountdown) {
            countdown.reset(); // back to READY at the configured duration
            Log.println("Countdown reset");
        } else {
            stopwatch.reset();
            Log.println("Stopwatch reset");
        }
        return;
    }

    if (isCountdown && countdown.phase != timerlogic::TIMER_RUNNING &&
        countdown.phase != timerlogic::TIMER_PAUSED) {
        for (int i = 0; i < 4; i++) {
            if (timerBtnHit(BTN_ADJ[i], x, y)) {
                // Session-only adjustment - deliberately NOT saved to flash;
                // the persisted default changes only from the web settings.
                countdown.adjustDurationMs(ADJ_DELTA_MS[i]);
                touchSuppressedUntilRelease = true;
                return;
            }
        }
    }

    // Anywhere else: deliberately inert - the timer faces have no invisible
    // touch zones, so a stray tap cannot dim the screen or switch pages.
}

/*-------- Status (/api/status) ----------*/

const char *stopwatchStateName()
{
    return timerlogic::timerPhaseName(stopwatch.phase);
}

uint32_t stopwatchElapsedSec()
{
    return (uint32_t)(stopwatch.elapsedMs(monoMs()) / 1000ULL);
}

const char *countdownStateName()
{
    return timerlogic::timerPhaseName(countdown.phase);
}

uint32_t countdownConfiguredSec()
{
    return (uint32_t)(countdown.durationMs / 1000ULL);
}

uint32_t countdownRemainingSec()
{
    return (uint32_t)((countdown.remainingMs(monoMs()) + 999ULL) / 1000ULL);
}
