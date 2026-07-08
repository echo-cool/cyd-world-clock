#ifndef BRIGHTNESS_H
#define BRIGHTNESS_H

#include <Arduino.h>

// Backlight PWM range used everywhere the UI accepts, stores or displays
// brightness. Keeping the range in one place prevents the web, touch and
// serial controls from drifting apart.
static const int BRIGHTNESS_MIN = 1;
static const int BRIGHTNESS_MAX = 255;
static const char *BRIGHTNESS_RANGE_TEXT = "1-255";

inline int clampBrightness(int value)
{
    return constrain(value, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
}

inline int brightnessPercent(int value)
{
    return map(clampBrightness(value), BRIGHTNESS_MIN, BRIGHTNESS_MAX, 0, 100);
}

#endif // BRIGHTNESS_H
