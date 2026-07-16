/*-------- Backlight / brightness control ----------*/
// Moved verbatim out of ClockLogic.cpp: the manual-brightness hold, the LDR
// auto-brightness (with its time-of-day fallback), the brightness bar overlay
// and the held-finger gesture step.

#include "brightnessControl.h"

#include "ClockLogic.h"    // tft, screen dimensions, clockBackgroundColor, CLOCK_DEBUG_PRINTLN
#include "brightness.h"    // clampBrightness / setBacklight / brightnessPercent
#include "projectConfig.h" // autoBrightness switch, night window, saved level

// Global variables for touch and backlight control
int backlightLevel = 80; // PWM value (0-255)

// Brightness bar state (globals so the touch UI can reset them cleanly)
unsigned long brightnessBarShownTime = 0;
bool brightnessBarVisible = false;

// Manual brightness override: when the user changes brightness (touch or serial),
// auto-brightness is suspended for MANUAL_BRIGHTNESS_HOLD_MS so the two don't
// fight. Stored as hold-start + active flag and compared with elapsed time,
// which stays correct across the 49.7-day millis() rollover (an absolute
// "until" deadline would not - this clock runs 24/7).
static bool manualBrightnessHold = false;
static unsigned long manualBrightnessSetMs = 0;

void markManualBrightness()
{
    manualBrightnessHold = true;
    manualBrightnessSetMs = millis();
}

bool manualBrightnessActive()
{
    if (!manualBrightnessHold) return false;
    if (millis() - manualBrightnessSetMs >= MANUAL_BRIGHTNESS_HOLD_MS) {
        manualBrightnessHold = false;
        return false;
    }
    return true;
}

unsigned long manualBrightnessRemainingMs()
{
    if (!manualBrightnessActive()) return 0;
    return MANUAL_BRIGHTNESS_HOLD_MS - (millis() - manualBrightnessSetMs);
}

/*-------- Ambient-light auto-brightness ----------*/
// Primary source: the CYD's onboard LDR (self-calibrating, see below).
// Fallback while the sensor is unproven (or disabled): the old fixed schedule
// on home-zone time. Manual changes (touch / settings / serial) always win
// for MANUAL_BRIGHTNESS_HOLD_MS via markManualBrightness().

#if USE_LDR_AUTOBRIGHTNESS
static int ldrLastRaw = 0;    // last raw sample, normalized so higher = darker
static float ldrEma = -1.0f;  // smoothed reading (-1 until first sample)
static int ldrMinSeen = 4095; // observed range since boot - used both to
static int ldrMaxSeen = 0;    // self-calibrate and to prove the sensor works
static bool ldrDark = false;

// Sample every couple of seconds and classify the room as dark/bright with
// hysteresis around the midpoint of the observed range. The LDR circuit on
// some CYD revisions is broken (flat readings); until the range has swung by
// LDR_MIN_SWING the sensor is not trusted and ldrIsTrusted() stays false.
static void ldrSample(unsigned long now)
{
    static unsigned long lastSample = 0;
    if (now - lastSample < 2000) return;
    lastSample = now;

    int raw = analogRead(LDR_PIN);
#if !LDR_DARK_IS_HIGH
    raw = 4095 - raw; // normalize: higher = darker
#endif
    ldrLastRaw = raw;
    ldrEma = (ldrEma < 0) ? raw : ldrEma + 0.2f * (raw - ldrEma);

    int e = (int)ldrEma;
    if (e < ldrMinSeen) ldrMinSeen = e;
    if (e > ldrMaxSeen) ldrMaxSeen = e;

    if (ldrMaxSeen - ldrMinSeen >= LDR_MIN_SWING) {
        int mid = (ldrMinSeen + ldrMaxSeen) / 2;
        int band = (ldrMaxSeen - ldrMinSeen) / 8; // hysteresis, no flapping
        if (!ldrDark && e > mid + band) {
            ldrDark = true;
            Log.println("Auto brightness: room went dark (LDR " + String(e) + ")");
        } else if (ldrDark && e < mid - band) {
            ldrDark = false;
            Log.println("Auto brightness: room went bright (LDR " + String(e) + ")");
        }
    }
}

static bool ldrIsTrusted()
{
    return ldrMaxSeen - ldrMinSeen >= LDR_MIN_SWING;
}
#endif // USE_LDR_AUTOBRIGHTNESS

void printLdrStatus()
{
#if USE_LDR_AUTOBRIGHTNESS
    Log.println("=== LDR (ambient light sensor, GPIO " + String(LDR_PIN) + ") ===");
    Log.println("Raw (normalized, higher = darker): " + String(ldrLastRaw));
    Log.println("Smoothed: " + String((int)ldrEma));
    Log.println("Range seen since boot: " + String(ldrMinSeen) + " - " + String(ldrMaxSeen));
    if (ldrIsTrusted()) {
        Log.println("Sensor trusted: yes | Room: " + String(ldrDark ? "DARK" : "BRIGHT"));
    } else {
        Log.println("Sensor trusted: not yet (swing < " + String(LDR_MIN_SWING) +
                       " counts; using the " + String(projectConfig.nightStartHour) +
                       "-" + String(projectConfig.nightEndHour) +
                       "h night window fallback). Cover the sensor");
        Log.println("or shine a light at it - if the value never moves, this board's");
        Log.println("LDR circuit is one of the known-bad CYD revisions.");
    }
#else
    Log.println("LDR auto-brightness disabled at compile time (USE_LDR_AUTOBRIGHTNESS=0)");
#endif
}

bool getLdrState(bool &trusted, bool &dark, int &smoothed)
{
#if USE_LDR_AUTOBRIGHTNESS
    trusted = ldrIsTrusted();
    dark = ldrDark;
    smoothed = (int)ldrEma;
    return true;
#else
    trusted = false;
    dark = false;
    smoothed = -1;
    return false;
#endif
}

void adjustBrightnessAuto()
{
    unsigned long currentTime = millis();

    // Don't fight a manual brightness change (touch / serial) - let it hold first
    if (manualBrightnessActive()) {
        return;
    }

    static unsigned long lastUpdate = 0;
    if (currentTime - lastUpdate < 250) return;
    lastUpdate = currentTime;

    int target = -1;
    int nightTarget = clampBrightness(projectConfig.nightBrightness);
    int dayTarget = clampBrightness(projectConfig.brightness);

#if USE_LDR_AUTOBRIGHTNESS
    // Sample even while auto-brightness is switched off so the sensor stays
    // calibrated (and the status pages stay live) for when it's re-enabled.
    ldrSample(currentTime);
#endif

    // Master switch (web settings page): off = ignore the light sensor and
    // the night schedule, hold the user's set brightness (fading back to it
    // if a dim was in effect when the switch was flipped).
    if (!projectConfig.autoBrightness) {
        target = dayTarget;
    }
#if USE_LDR_AUTOBRIGHTNESS
    else if (ldrIsTrusted()) {
        target = ldrDark ? nightTarget : dayTarget;
    }
#endif

    // Fallback: schedule on home-zone time. The window is configurable from
    // the web settings page (default 1-7 AM) and may wrap midnight; equal
    // start/end hours disable it.
    if (target < 0) {
        if (!worldZones[0].initialized) return;
        int currentHour = hour(worldZones[0].tz.now());
        int s = projectConfig.nightStartHour;
        int e = projectConfig.nightEndHour;
        bool night = (s != e) &&
                     (s < e ? (currentHour >= s && currentHour < e)
                            : (currentHour >= s || currentHour < e));
        target = night ? nightTarget : dayTarget;
    }

    // Fade toward the target instead of jumping (full sweep in a few seconds)
    if (backlightLevel != target) {
        int step = 8;
        if (backlightLevel < target) {
            backlightLevel = min(backlightLevel + step, target);
        } else {
            backlightLevel = max(backlightLevel - step, target);
        }
        setBacklight(backlightLevel);
    }
}

void showBrightnessBar(int brightness)
{
    // Draw brightness bar in center of screen
    int barWidth = min(260, screenWidth - 80);
    int barHeight = 20;
    int centerX = screenWidth / 2;
    int barX = (screenWidth - barWidth) / 2;
    int barY = (screenHeight - barHeight) / 2;

    // Clear area around the bar
    tft.fillRect(barX - 10, barY - 30, barWidth + 20, barHeight + 60, clockBackgroundColor);

    // Draw "BRIGHTNESS" label
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString("BRIGHTNESS", centerX, barY - 20);

    // Draw outer border
    tft.drawRect(barX - 2, barY - 2, barWidth + 4, barHeight + 4, TFT_WHITE);

    // Fill background (empty part)
    tft.fillRect(barX, barY, barWidth, barHeight, TFT_BLACK);

    // Calculate fill width based on the supported brightness range.
    int fillWidth = map(clampBrightness(brightness), BRIGHTNESS_MIN, BRIGHTNESS_MAX, 0, barWidth);

    // Draw filled portion with gradient-like effect
    uint16_t fillColor;
    if (brightness < 85) {
        fillColor = TFT_RED;     // Low brightness - red
    } else if (brightness < 170) {
        fillColor = TFT_YELLOW;  // Medium brightness - yellow
    } else {
        fillColor = TFT_GREEN;   // High brightness - green
    }

    if (fillWidth > 0) {
        tft.fillRect(barX, barY, fillWidth, barHeight, fillColor);
    }

    // Draw percentage text
    int percentage = brightnessPercent(brightness);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(String(percentage) + "%", centerX, barY + barHeight + 10);
}

void brightnessGestureStep(int delta)
{
    // Debounce - one +/-1 step every 10 ms while the finger is held.
    static unsigned long lastStepMs = 0;
    unsigned long now = millis();
    if (now - lastStepMs <= 10) return;
    lastStepMs = now;

    backlightLevel = clampBrightness(backlightLevel + delta);
    setBacklight(backlightLevel);
    markManualBrightness(); // hold off auto-brightness
    showBrightnessBar(backlightLevel);
    brightnessBarVisible = true;
    brightnessBarShownTime = now;

    // Per-step logging is debug-only: a held finger would otherwise emit a
    // line every 10 ms and flush useful entries out of the log ring buffer.
    CLOCK_DEBUG_PRINTLN(String(delta < 0 ? "Dimmer: " : "Brighter: ") +
                        String(backlightLevel));
}
