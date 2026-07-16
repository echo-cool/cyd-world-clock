/*-------- Sun position / day-night phase ----------*/
// Moved verbatim out of ClockLogic.cpp: the NOAA solar-elevation math, the
// zone day phase and its colors/icons, sunrise/sunset lookup and the daylight
// gradient bar.

#include "solarPhase.h"

#include "ClockLogic.h"    // tft (color565), clockBackgroundColor
#include "dateMath.h"      // daysFromCivil (host-tested pure math)
#include "projectConfig.h" // dayNightIcons - readable night-colors option
#include "uiPages.h"       // getCityCoords - preset-city coordinates

// Sun elevation in degrees above the horizon at utc for a site, via NOAA's
// simplified solar position algorithm (Fourier-series equation of time and
// declination). Accurate to ~0.1 degrees - plenty for picking display colors.
static float solarElevationDeg(float latDeg, float lonDeg, time_t utc)
{
    int doy = (int)(daysFromCivil(year(utc), month(utc), day(utc)) -
                    daysFromCivil(year(utc), 1, 1)) + 1;
    float hourUtc = hour(utc) + minute(utc) / 60.0f;
    float g = 2.0f * PI / 365.0f * (doy - 1 + (hourUtc - 12.0f) / 24.0f);
    float eqtimeMin = 229.18f * (0.000075f + 0.001868f * cosf(g) - 0.032077f * sinf(g)
                                 - 0.014615f * cosf(2 * g) - 0.040849f * sinf(2 * g));
    float declRad = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g)
                    - 0.006758f * cosf(2 * g) + 0.000907f * sinf(2 * g)
                    - 0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
    float trueSolarMin = hourUtc * 60.0f + eqtimeMin + 4.0f * lonDeg;
    float hourAngleDeg = trueSolarMin / 4.0f - 180.0f;
    while (hourAngleDeg < -180.0f) hourAngleDeg += 360.0f;
    while (hourAngleDeg > 180.0f) hourAngleDeg -= 360.0f;
    float latRad = latDeg * DEG_TO_RAD;
    float cosZen = sinf(latRad) * sinf(declRad) +
                   cosf(latRad) * cosf(declRad) * cosf(hourAngleDeg * DEG_TO_RAD);
    if (cosZen > 1.0f) cosZen = 1.0f;
    if (cosZen < -1.0f) cosZen = -1.0f;
    return 90.0f - acosf(cosZen) * RAD_TO_DEG;
}

DayPhase zoneDayPhase(WorldClockZone &zone)
{
    time_t local = zone.tz.now();
    int hr = hour(local);

    float lat, lon;
    if (getCityCoords(zone.timezone, lat, lon)) {
        // True day/night from the sun's actual position (so London reads as
        // night at 4:30 PM in December and as day at 9 PM in June). -0.833
        // degrees is the standard sunrise/sunset threshold: the sun's upper
        // limb on the horizon, corrected for atmospheric refraction.
        if (solarElevationDeg(lat, lon, UTC.now()) > -0.833f) {
            return PHASE_DAY;
        }
        return hr >= 12 ? PHASE_EVENING : PHASE_NIGHT;
    }

    // Zone outside the preset list (no coordinates): fixed windows, as before
    if (hr >= 6 && hr < 18) return PHASE_DAY;
    return hr >= 18 ? PHASE_EVENING : PHASE_NIGHT;
}

uint16_t getDayNightColor(WorldClockZone &zone)
{
    bool readable = projectConfig.dayNightIcons;
    switch (zoneDayPhase(zone)) {
    case PHASE_DAY: return TFT_ORANGE;
    case PHASE_EVENING: return readable ? EVENING_TEXT_COLOR : TFT_LIGHTGREY;
    default: return readable ? NIGHT_TEXT_COLOR : TFT_DARKGREY;
    }
}

uint16_t getDayNightLabelColor(WorldClockZone &zone)
{
    bool readable = projectConfig.dayNightIcons;
    switch (zoneDayPhase(zone)) {
    case PHASE_DAY: return TFT_YELLOW;
    case PHASE_EVENING: return readable ? EVENING_TEXT_COLOR : TFT_LIGHTGREY;
    default: return readable ? NIGHT_TEXT_COLOR : TFT_DARKGREY;
    }
}

bool zoneIsNight(WorldClockZone &zone)
{
    return zoneDayPhase(zone) != PHASE_DAY;
}

// ~12px sun (day) or crescent moon (evening/night) so the day/night state
// doesn't ride on text color alone. Sized/positioned to clear the longest
// centered city names (e.g. SANTA CLARA starts at quadrant x=14).
void drawDayNightIcon(TFT_eSPI &gfx, int cx, int cy, DayPhase phase)
{
    if (phase == PHASE_DAY) {
        gfx.fillCircle(cx, cy, 3, TFT_YELLOW);
        static const int8_t rays[8][4] = {
            {5, 0, 6, 0}, {-5, 0, -6, 0}, {0, 5, 0, 6}, {0, -5, 0, -6},
            {4, 4, 5, 5}, {-4, 4, -5, 5}, {4, -4, 5, -5}, {-4, -4, -5, -5}};
        for (int i = 0; i < 8; i++) {
            gfx.drawLine(cx + rays[i][0], cy + rays[i][1],
                         cx + rays[i][2], cy + rays[i][3], TFT_YELLOW);
        }
    } else {
        uint16_t c = (phase == PHASE_EVENING) ? EVENING_TEXT_COLOR : NIGHT_TEXT_COLOR;
        gfx.fillCircle(cx, cy, 4, c);
        gfx.fillCircle(cx + 3, cy - 1, 4, clockBackgroundColor); // carve the crescent
    }
}

// Public wrapper for the alternate faces: the same sun/moon glyph keyed by
// the zone's current phase, without computing the DayPhase themselves.
void drawZoneDayNightIcon(TFT_eSPI &gfx, int cx, int cy, WorldClockZone &zone)
{
    drawDayNightIcon(gfx, cx, cy, zoneDayPhase(zone));
}

// Today's local sunrise/sunset for a zone as minutes-of-day, found by
// scanning the sun's elevation across the local day in 2-minute steps (the
// same -0.833 degree horizon the day/night colors use). False when the zone
// has no preset coordinates or the sun never crosses the horizon today
// (polar day/night). Cheap enough for once-a-day repaints, not per frame.
bool zoneSunTimes(WorldClockZone &zone, int &riseMin, int &setMin)
{
    float lat, lon;
    if (!getCityCoords(zone.timezone, lat, lon)) return false;

    time_t local = zone.tz.now();
    time_t utcNow = UTC.now();
    long secOfDay = (long)hour(local) * 3600 + (long)minute(local) * 60 + second(local);

    riseMin = -1;
    setMin = -1;
    bool prevUp = solarElevationDeg(lat, lon, utcNow - secOfDay) > -0.833f;
    for (int m = 2; m <= 1440; m += 2) {
        time_t colUtc = utcNow + ((long)m * 60 - secOfDay);
        bool up = solarElevationDeg(lat, lon, colUtc) > -0.833f;
        if (up && !prevUp && riseMin < 0) riseMin = m;
        if (!up && prevUp && setMin < 0) setMin = m;
        prevUp = up;
    }
    return riseMin >= 0 && setMin >= 0;
}

// Color for the daylight bar at a given solar elevation: deep blue night,
// dark blue twilight, orange around the horizon, warm yellow midday.
// Piecewise-linear blend between the anchor stops.
static uint16_t daylightBarColor(float elevDeg)
{
    static const struct { float e; uint8_t r, g, b; } stops[] = {
        {-18.0f, 8, 10, 35},    // astronomical night
        {-6.0f, 30, 40, 90},    // civil twilight
        {0.0f, 200, 90, 25},    // sunrise / sunset
        {12.0f, 255, 160, 30},  // low sun
        {40.0f, 255, 225, 90},  // high sun
    };
    const int n = sizeof(stops) / sizeof(stops[0]);
    if (elevDeg <= stops[0].e) return tft.color565(stops[0].r, stops[0].g, stops[0].b);
    for (int i = 1; i < n; i++) {
        if (elevDeg <= stops[i].e) {
            float f = (elevDeg - stops[i - 1].e) / (stops[i].e - stops[i - 1].e);
            uint8_t r = stops[i - 1].r + f * (stops[i].r - stops[i - 1].r);
            uint8_t g = stops[i - 1].g + f * (stops[i].g - stops[i - 1].g);
            uint8_t b = stops[i - 1].b + f * (stops[i].b - stops[i - 1].b);
            return tft.color565(r, g, b);
        }
    }
    return tft.color565(stops[n - 1].r, stops[n - 1].g, stops[n - 1].b);
}

// Daylight gradient bar: the zone's local 00:00-24:00 mapped left to
// right, each column colored by the sun's real elevation at that moment
// today (same solar math as the day/night colors), with a white tick at the
// current time. Shows at a glance how deep into day or night each city is.
// Skipped for zones without preset coordinates. Shared with the big-clock
// face (clockFaces.cpp), which draws it thicker than the quadrants' 3px.
void renderDaylightBar(TFT_eSPI &gfx, int x, int y, int w, WorldClockZone &zone,
                       int h)
{
    float lat, lon;
    if (!getCityCoords(zone.timezone, lat, lon)) return;

    time_t local = zone.tz.now();
    time_t utcNow = UTC.now();
    long secOfDay = (long)hour(local) * 3600 + (long)minute(local) * 60 + second(local);

    for (int i = 0; i < w; i++) {
        long colSec = (long)i * 86400L / (w - 1);
        time_t colUtc = utcNow + (colSec - secOfDay);
        gfx.drawFastVLine(x + i, y, h, daylightBarColor(solarElevationDeg(lat, lon, colUtc)));
    }

    // "Now" tick, with background-color gaps so it reads inside the bright
    // midday section too
    int tickX = x + (int)(secOfDay * (long)(w - 1) / 86400L);
    gfx.drawFastVLine(tickX - 1, y - 2, h + 4, clockBackgroundColor);
    gfx.drawFastVLine(tickX + 1, y - 2, h + 4, clockBackgroundColor);
    gfx.drawFastVLine(tickX, y - 2, h + 4, TFT_WHITE);
}
