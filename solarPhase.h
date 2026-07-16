#ifndef SOLAR_PHASE_H
#define SOLAR_PHASE_H

// Sun-position / day-night logic: the zone day phase and its display colors,
// the sun/moon icon, sunrise/sunset lookup and the daylight gradient bar
// (all driven by the same NOAA solar-elevation math). Moved out of
// ClockLogic.cpp/.h; the preset-city coordinates come from getCityCoords
// (uiPages.cpp).

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "worldZones.h"

// Cool "night sky" blues used for evening/night text when the readable
// night-colors option (projectConfig.dayNightIcons) is on. The legacy greys
// encoded the phase in brightness, but dark grey digits on the black
// background - on top of the auto-dimmed backlight - made the time hardest
// to read exactly when it's glanced at half-awake. With the option on, the
// sun/moon icon carries the day/night meaning instead, so the text can stay
// readable: warm by day, cool blue at night. (The crescent-moon weather
// glyph in ClockLogic.cpp reuses the evening shade.)
const uint16_t EVENING_TEXT_COLOR = 0x965F; // light ice blue
const uint16_t NIGHT_TEXT_COLOR = 0x7D5D;   // dimmer steel blue

enum DayPhase
{
    PHASE_DAY,     // sun above the horizon
    PHASE_EVENING, // sun down, before local midnight
    PHASE_NIGHT    // sun down, small hours
};

// Current day phase for a zone: true sun position for preset cities with
// coordinates, fixed 6AM-6PM windows otherwise.
DayPhase zoneDayPhase(WorldClockZone &zone);

// Day/night-dependent colors for a zone's local time (used for time digits
// and for labels/dates respectively). Preset cities carry coordinates, so
// day/night follows the sun's real position (sunrise/sunset incl. seasons);
// zones without known coordinates fall back to fixed 6AM-6PM windows.
uint16_t getDayNightColor(WorldClockZone &zone);
uint16_t getDayNightLabelColor(WorldClockZone &zone);

// True when the sun is down in a zone (evening or night phase).
bool zoneIsNight(WorldClockZone &zone);

// ~12px sun (day) or crescent moon (evening/night) centered on (cx, cy),
// keyed by an already-computed phase (the quadrant renderer reuses its
// per-frame zoneDayPhase result).
void drawDayNightIcon(TFT_eSPI &gfx, int cx, int cy, DayPhase phase);

// ~12px sun or crescent moon showing the zone's current day/night phase,
// centered on (cx, cy). Same glyph as the quadrant corner icons.
void drawZoneDayNightIcon(TFT_eSPI &gfx, int cx, int cy, WorldClockZone &zone);

// Today's local sunrise/sunset for a zone as minutes-of-day (e.g. 358 =
// 05:58). False when the zone has no preset coordinates or the sun never
// crosses the horizon today (polar day/night).
bool zoneSunTimes(WorldClockZone &zone, int &riseMin, int &setMin);

// Daylight gradient bar: the zone's local day mapped left to right, colored
// by real solar elevation, with a white tick at the current time. `h` is the
// bar thickness (the quadrants use the 3px default; the big face draws it
// taller). No-op for zones without preset coordinates.
void renderDaylightBar(TFT_eSPI &gfx, int x, int y, int w, WorldClockZone &zone,
                       int h = 3);

#endif // SOLAR_PHASE_H
