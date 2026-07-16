#ifndef BRIGHTNESS_H
#define BRIGHTNESS_H

#include <Arduino.h>

#include "boardProfile.h" // BOARD_BACKLIGHT_PIN

// Backlight PWM range used everywhere the UI accepts, stores or displays
// brightness. Keeping the range in one place prevents the web, touch and
// serial controls from drifting apart.
constexpr int BRIGHTNESS_MIN = 1;
constexpr int BRIGHTNESS_MAX = 255;
constexpr const char *BRIGHTNESS_RANGE_TEXT = "1-255";

inline int clampBrightness(int value)
{
    return constrain(value, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
}

inline int brightnessPercent(int value)
{
    return map(clampBrightness(value), BRIGHTNESS_MIN, BRIGHTNESS_MAX, 0, 100);
}

// One-time pin setup for the backlight PWM output (call from display setup).
inline void backlightPinSetup()
{
    pinMode(BOARD_BACKLIGHT_PIN, OUTPUT);
}

// The single write path to the backlight PWM: every control surface (touch
// gestures, serial command, web settings, auto-brightness) goes through here
// so the pin and the clamp live in one place.
inline void setBacklight(int value)
{
    analogWrite(BOARD_BACKLIGHT_PIN, clampBrightness(value));
}

#endif // BRIGHTNESS_H
