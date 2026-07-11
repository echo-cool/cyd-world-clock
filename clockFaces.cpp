#include "clockFaces.h"

#include "ClockLogic.h"
#include "holidayService.h" // holiday markers on the calendar face
#include "projectConfig.h"
#include "uiPages.h" // getMarketInfoForTimezone, getPosixFallback
#include "weatherService.h"

static const char *DAY_NAMES[8] = {"", "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *MONTH_ABBR[13] = {"", "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                     "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

const char *clockFaceName(int face)
{
    switch (face) {
    case FACE_BIG: return "Big clock";
    case FACE_CALENDAR: return "Calendar";
    case FACE_WEATHER: return "Weather";
    case FACE_MARKETS: return "Markets";
    default: return "World clock";
    }
}

/*-------- Shared formatting helpers ----------*/
// (formatHHMM lives in ClockLogic.cpp - shared with the quadrant renderer)

static String formatDate(time_t local)
{
    char buf[10];
    if (NOT_US_DATE) {
        sprintf(buf, "%02d/%02d/%02d", day(local), month(local), year(local) % 100);
    } else {
        sprintf(buf, "%02d/%02d/%02d", month(local), day(local), year(local) % 100);
    }
    return String(buf);
}

static int daysInMonth(int y, int m)
{
    static const int dm[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m];
}

/*-------- Screen scaling ----------*/
// The alternate faces were laid out on the 320x240 CYD. sx()/sy() map those
// design coordinates onto the actual panel - identity on the CYD, 1.5x/1.33x
// on the Hosyond 4.0" 480x320 - with the same round-half-up the touch UI's
// scaleUiX/Y uses. largeFace() gates the font-size upgrades that keep the
// scaled layouts from looking sparse; same screen class as the quad face's
// large layout (useLargeQuadrantLayout).

static int sx(int v)
{
    return (v * screenWidth + 160) / 320;
}

static int sy(int v)
{
    return (v * screenHeight + 120) / 240;
}

static bool largeFace()
{
    return screenWidth >= 440 && screenHeight >= 300;
}

/*-------- Big clock face ----------*/
// Home zone in 75px digits with the same at-a-glance extras as the quadrant
// face: day/night icon, current weather in the header corner, gold holiday
// line, daylight gradient bar, and an alert / precipitation / market status
// line. A mini strip of the other three zones (with day offsets and temps)
// runs along the bottom.

// Day offset of a zone vs home as " +1" / " -1", or "" when on the same
// calendar date. Compares real civil dates so month/year edges stay correct.
static String zoneDayOffset(int k)
{
    time_t local = worldZones[k].tz.now();
    time_t home = worldZones[0].tz.now();
    long diff = daysFromCivil(year(local), month(local), day(local)) -
                daysFromCivil(year(home), month(home), day(home));
    if (diff >= 1) return " +1";
    if (diff <= -1) return " -1";
    return "";
}

// Bar color for one 15-minute forecast step, from its WMO code: white snow,
// orange thunderstorm, cyan rain/drizzle - matching the notice-text colors.
static uint16_t precipBarColor(int code)
{
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return TFT_WHITE;
    if (code >= 95) return TFT_ORANGE;
    return TFT_CYAN;
}

// 4-hour precipitation chart for the home zone in the big face's status
// slot: one bar per 15-minute Open-Meteo forecast step over a baseline with
// "NOW / +1H / +2H / +3H" labels, bar heights scaled to the wettest step
// (floored at 1 mm so drizzle reads low) with the scale value in the right
// margin. Draws nothing and returns false when there is no precipitation in
// the window (or no data yet), so the caller falls back to the status texts.
static bool drawHomePrecipChart(int x, int y, int w)
{
    float mm[PRECIP_FORECAST_STEPS];
    uint8_t codes[PRECIP_FORECAST_STEPS];
    int n = getZonePrecip15(0, mm, codes, PRECIP_FORECAST_STEPS);
    if (n <= 0) return false;

    float maxV = 0;
    for (int i = 0; i < n; i++) {
        if (mm[i] > maxV) maxV = mm[i];
    }
    if (maxV < 0.1f) return false; // dry (or trace) window - no chart

    const int plotH = sy(18);
    int baseline = y + plotH;
    int step = w / PRECIP_FORECAST_STEPS;
    float scale = max(maxV, 1.0f); // mm that fills the plot height

    tft.drawFastHLine(x, baseline, w, TFT_DARKGREY);
    for (int k = 1; k <= 3; k++) { // hour ticks under the baseline
        tft.drawFastVLine(x + k * 4 * step, baseline + 1, 2, TFT_DARKGREY);
    }

    for (int i = 0; i < n; i++) {
        if (mm[i] <= 0.0f) continue;
        int h = (int)(mm[i] / scale * plotH + 0.5f);
        if (h < 1) h = 1;
        if (h > plotH) h = plotH;
        tft.fillRect(x + i * step, baseline - h, step - 2, h, precipBarColor(codes[i]));
    }

    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("NOW", x, baseline + 2);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("+1H", x + 4 * step, baseline + 2);
    tft.drawString("+2H", x + 8 * step, baseline + 2);
    tft.drawString("+3H", x + 12 * step, baseline + 2);

    // Scale (mm per 15 min that fills the plot) in the free right margin
    tft.setTextDatum(TL_DATUM);
    tft.drawString(String(scale, 1), x + w + 6, y + 1);
    tft.drawString("MM", x + w + 6, y + 10);
    return true;
}

static void renderBigClockFace(bool full)
{
    time_t local = worldZones[0].tz.now();
    uint16_t timeColor = getDayNightColor(worldZones[0]);
    int cx = screenWidth / 2;

    if (full) {
        tft.fillScreen(clockBackgroundColor);

        // City name: font 4 while it fits between the header corners, longer
        // names drop to font 2 instead of truncating.
        tft.setTextFont(4);
        tft.setTextSize(1);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_WHITE, clockBackgroundColor);
        if (tft.textWidth(worldZones[0].name) <= sx(150)) {
            tft.drawString(worldZones[0].name, cx, 6);
        } else {
            tft.setTextFont(2);
            tft.drawString(fitTextToWidth(tft, worldZones[0].name, sx(150)), cx, 10);
        }

        tft.drawFastHLine(sx(10), sy(196), sx(300), TFT_DARKGREY);
    }

    // Header left corner: sun/moon phase icon. Always on for this face - the
    // corner is dedicated space, unlike the quadrants where the icon is an
    // opt-in extra.
    tft.fillRect(0, 0, sx(70), sy(36), clockBackgroundColor);
    drawZoneDayNightIcon(tft, sx(18), sy(18), worldZones[0]);

    // Header right corner: current temperature + condition icon for home
    tft.fillRect(sx(236), 0, screenWidth - sx(236), sy(40), clockBackgroundColor);
    ZoneWeather homeWx = getZoneWeather(0);
    if (projectConfig.quadWeather && homeWx.valid) {
        String temp = String(displayTemp(homeWx.tempC));
        tft.setTextFont(4);
        tft.setTextSize(1);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(TFT_WHITE, clockBackgroundColor);
        tft.drawString(temp, sx(306), 6);
        tft.drawCircle(sx(306) + 5, 12, 3, TFT_WHITE); // degree mark
        drawWeatherIcon(tft, sx(306) - tft.textWidth(temp) - 16, sy(18),
                        homeWx.weatherCode, zoneIsNight(worldZones[0]));
    }

    // Big HH:MM (Font 8 carries only digits ':' '-' '.', so AM/PM is drawn
    // separately in a text font). The 75px digits sit a little lower in the
    // large screen's taller slot so they stay visually centered.
    bool pm;
    String hhmm = formatHHMM(local, pm);
    int timeY = sy(46) + (largeFace() ? 14 : 0);
    tft.fillRect(0, sy(44), screenWidth, sy(80), clockBackgroundColor);
    tft.setTextFont(8);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(timeColor, clockBackgroundColor);
    tft.drawString(hhmm, cx, timeY);
    int timeWidth = tft.textWidth(hhmm); // measured while font 8 is active

    if (!SHOW_24HOUR) {
        tft.setTextFont(2);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(pm ? "PM" : "AM", cx + timeWidth / 2 + 6, timeY + 2);
    }

    // Day + date; a public holiday turns the line gold with the holiday's
    // name appended, like the quadrant day line.
    tft.fillRect(0, sy(130), screenWidth, sy(28), clockBackgroundColor);
    String dayDate = String(DAY_NAMES[weekday(local)]) + " " + formatDate(local);
    char holidayName[32];
    uint32_t todayYmd = (uint32_t)year(local) * 10000u +
                        (uint32_t)month(local) * 100u + (uint32_t)day(local);
    tft.setTextDatum(TC_DATUM);
    if (getHolidayName(0, todayYmd, holidayName, sizeof(holidayName))) {
        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.setTextColor(TFT_GOLD, clockBackgroundColor);
        tft.drawString(fitTextToWidth(tft, dayDate + " - " + holidayName, sx(312)),
                       cx, sy(136));
    } else {
        tft.setTextFont(4);
        tft.setTextSize(1);
        tft.setTextColor(getDayNightLabelColor(worldZones[0]), clockBackgroundColor);
        tft.drawString(dayDate, cx, sy(132));
    }

    // Daylight gradient bar: home's whole local day with a "now" tick.
    // Always on - this face has a dedicated slot for it, so it never shifts
    // the layout the way the quadrant option does.
    tft.fillRect(0, sy(158), screenWidth, sy(12), clockBackgroundColor);
    renderDaylightBar(tft, cx - sx(120), sy(161), sx(240), worldZones[0]);

    // Status area: an official NWS alert wins; otherwise the 4-hour
    // precipitation chart when rain/snow is in the 15-minute forecast (a
    // derived STORM/HEAVY RAIN alert would only repeat what the chart's
    // colored bars show); then the derived alert, the textual notice (a
    // fallback for trace precipitation the chart filters out), and finally
    // the market status of the home zone (computed fresh - the
    // once-per-minute cache in hasTimeChanged only runs while the quad face
    // is active).
    tft.fillRect(0, sy(168), screenWidth, sy(27), clockBackgroundColor);
    String alert = projectConfig.weatherAlerts ? getZoneAlert(0) : String("");
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    if (alert.length() > 0 && getZoneAlertOfficial(0)) {
        tft.setTextColor(WX_ALERT_COLOR, clockBackgroundColor);
        tft.drawString(fitTextToWidth(tft, alert, sx(312)), cx, sy(174));
    } else if (projectConfig.quadWeather &&
               drawHomePrecipChart(cx - sx(120), sy(168), sx(240))) {
        // chart drawn
    } else if (alert.length() > 0) {
        tft.setTextColor(WX_ALERT_COLOR, clockBackgroundColor);
        tft.drawString(fitTextToWidth(tft, alert, sx(312)), cx, sy(174));
    } else {
        String notice = projectConfig.quadWeather ? getZonePrecipNotice(0) : String("");
        if (notice.length() > 0) {
            tft.setTextColor(weatherNoticeColor(notice), clockBackgroundColor);
            tft.drawString(notice, cx, sy(174));
        } else if (worldZones[0].market.hasMarket) {
            String status = getMarketStatus(worldZones[0]);
            tft.setTextColor(getMarketStatusColor(status), clockBackgroundColor);
            tft.drawString(status, cx, sy(174));
        }
    }

    // Mini strip: the other three zones, each with day offset and temperature.
    // The large screen bumps the name/time fonts a step so the strip doesn't
    // read as fine print on the 4" panel.
    bool big = largeFace();
    int nameY = sy(204) + (big ? -2 : 0);
    int stripTimeY = big ? nameY + 19 : 216;
    tft.fillRect(0, sy(202), screenWidth, screenHeight - sy(202), clockBackgroundColor);
    for (int k = 1; k < 4; k++) {
        int colCx = sx(60 + (k - 1) * 100);
        String name = worldZones[k].name;
        if (name.length() > 12) name = name.substring(0, 12);
        String offset = zoneDayOffset(k);

        // Name (+1/-1 vs home in grey), centered as one composed line
        tft.setTextFont(big ? 2 : 1);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        int nameW = tft.textWidth(name);
        int offW = offset.length() ? tft.textWidth(offset) : 0;
        int nx = colCx - (nameW + offW) / 2;
        tft.setTextColor(getDayNightLabelColor(worldZones[k]), clockBackgroundColor);
        tft.drawString(name, nx, nameY);
        if (offW > 0) {
            tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
            tft.drawString(offset, nx + nameW, nameY);
        }

        // Time plus the zone's current temperature, centered together (the
        // temp stays font 2 next to the large screen's font-4 time).
        bool zonePm;
        String zoneTime = formatHHMM(worldZones[k].tz.now(), zonePm);
        if (!SHOW_24HOUR) zoneTime += zonePm ? "P" : "A";
        ZoneWeather w = getZoneWeather(k);
        String temp = (projectConfig.quadWeather && w.valid)
                          ? String(displayTemp(w.tempC)) : String("");
        tft.setTextFont(big ? 4 : 2);
        tft.setTextDatum(TL_DATUM);
        int timeW = tft.textWidth(zoneTime);
        tft.setTextFont(2);
        int tempW = temp.length() ? tft.textWidth(temp) + 10 : 0;
        int tx = colCx - (timeW + tempW) / 2;
        tft.setTextFont(big ? 4 : 2);
        tft.setTextColor(getDayNightColor(worldZones[k]), clockBackgroundColor);
        tft.drawString(zoneTime, tx, stripTimeY);
        if (tempW > 0) {
            int tempY = stripTimeY + (big ? 6 : 0);
            tft.setTextFont(2);
            tft.setTextColor(TFT_WHITE, clockBackgroundColor);
            tft.drawString(temp, tx + timeW + 6, tempY);
            tft.drawCircle(tx + timeW + 6 + tft.textWidth(temp) + 3, tempY + 3, 2,
                           TFT_WHITE);
        }
    }
}

/*-------- Calendar face ----------*/
// Month grid for the home zone with today highlighted; clock and current
// weather in the header. Public holidays are marked in gold, with today's /
// the next holiday's name in a footer line between the home zone's sunrise
// and sunset times. Leading/trailing grid cells show the adjacent months'
// days dimmed, so week rows always read complete.

// Dim grey for the adjacent-month days - darker than the weekend grey so the
// current month stays visually dominant.
static const uint16_t ADJACENT_MONTH_COLOR = 0x39E7;

// "H:MM" for a minutes-of-day value, honoring the 12/24-hour setting with a
// compact A/P suffix (footer-sized).
static String formatMinutesOfDay(int minOfDay)
{
    int h = (minOfDay / 60) % 24;
    int m = minOfDay % 60;
    char buf[8];
    if (SHOW_24HOUR) {
        sprintf(buf, "%d:%02d", h, m);
        return String(buf);
    }
    int h12 = h % 12;
    if (h12 == 0) h12 = 12;
    sprintf(buf, "%d:%02d", h12, m);
    return String(buf) + (h >= 12 ? "P" : "A");
}

// Name of the holiday on ymd, or nullptr. Scans the copied-out list.
static const char *holidayInList(const PublicHoliday *hols, int count, uint32_t ymd)
{
    for (int i = 0; i < count; i++) {
        if (hols[i].date == ymd) return hols[i].name;
    }
    return nullptr;
}

static void renderCalendarFace(bool redrawGrid, time_t local)
{
    int yr = year(local);
    int mo = month(local);
    int dd = day(local);
    bool big = largeFace();
    int daySize = big ? 3 : 2; // GLCD day numbers: 24px on the 4", 16px on the CYD

    if (redrawGrid) {
        tft.fillScreen(clockBackgroundColor);

        // Home zone's public holidays (async - may be empty until the first
        // fetch lands; the version check in drawAlternateFace repaints then)
        static PublicHoliday hols[32];
        int holCount = getZoneHolidays(0, hols, 32);

        tft.setTextFont(4);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, clockBackgroundColor);
        tft.drawString(String(MONTH_ABBR[mo]) + " " + String(yr), sx(8), 4);

        static const char *WD_SUN[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
        static const char *WD_MON[7] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
        const char **WD = projectConfig.weekStartMonday ? WD_MON : WD_SUN;
        tft.setTextFont(1);
        tft.setTextSize(big ? 2 : 1);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_CYAN, clockBackgroundColor);
        // On the large screen the taller (size 2) header sits a touch higher
        // so it clears a row-0 today box (whose top is 3px above the row).
        int weekdayY = big ? sy(33) : sy(36);
        for (int c = 0; c < 7; c++) {
            tft.drawString(WD[c], sx(6 + c * 44 + 22), weekdayY);
        }

        // 1970-01-01 (daysFromCivil == 0) was a Thursday, so +4 lands the
        // week on 0 = Sunday; one more rotation makes 0 = Monday when the
        // week starts there.
        int firstDow = (int)((daysFromCivil(yr, mo, 1) + 4) % 7);
        if (projectConfig.weekStartMonday) firstDow = (firstDow + 6) % 7;
        int dim = daysInMonth(yr, mo);

        tft.setTextFont(1);
        tft.setTextSize(daySize);
        tft.setTextDatum(TC_DATUM);

        // Dimmed adjacent-month days in the otherwise-empty leading and
        // trailing cells, so every week row reads complete.
        tft.setTextColor(ADJACENT_MONTH_COLOR, clockBackgroundColor);
        int prevMo = (mo == 1) ? 12 : mo - 1;
        int prevDim = daysInMonth((mo == 1) ? yr - 1 : yr, prevMo);
        for (int c = 0; c < firstDow; c++) {
            tft.drawString(String(prevDim - firstDow + 1 + c),
                           sx(6 + c * 44 + 22), sy(48));
        }
        int gridRows = (firstDow + dim + 6) / 7;
        for (int cell = firstDow + dim; cell < gridRows * 7; cell++) {
            tft.drawString(String(cell - (firstDow + dim) + 1),
                           sx(6 + (cell % 7) * 44 + 22), sy(48 + (cell / 7) * 30));
        }

        for (int d = 1; d <= dim; d++) {
            int cell = firstDow + d - 1;
            int col = cell % 7;
            int row = cell / 7;
            int cellCx = sx(6 + col * 44 + 22);
            int cy = sy(48 + row * 30);
            uint32_t ymd = (uint32_t)yr * 10000u + (uint32_t)mo * 100u + (uint32_t)d;
            bool holiday = holidayInList(hols, holCount, ymd) != nullptr;
            if (d == dd) {
                uint16_t boxColor = holiday ? TFT_GOLD : TFT_ORANGE;
                tft.fillRoundRect(sx(6 + col * 44 + 4), cy - 3, sx(36),
                                  big ? 30 : 22, big ? 7 : 5, boxColor);
                tft.setTextColor(TFT_BLACK, boxColor);
            } else if (holiday) {
                tft.setTextColor(TFT_GOLD, clockBackgroundColor);
            } else {
                bool weekend = projectConfig.weekStartMonday ? (col >= 5)
                                                             : (col == 0 || col == 6);
                tft.setTextColor(weekend ? TFT_DARKGREY : TFT_WHITE, clockBackgroundColor);
            }
            tft.drawString(String(d), cellCx, cy);
        }

        // Footer: today's holiday by name, otherwise the next upcoming one
        uint32_t todayYmd = (uint32_t)yr * 10000u + (uint32_t)mo * 100u + (uint32_t)dd;
        const char *todayName = holidayInList(hols, holCount, todayYmd);
        String footer;
        uint16_t footerColor = TFT_GOLD;
        if (todayName) {
            footer = "TODAY: " + String(todayName);
        } else {
            uint32_t bestDate = 0;
            const char *bestName = nullptr;
            for (int i = 0; i < holCount; i++) {
                if (hols[i].date > todayYmd && (bestDate == 0 || hols[i].date < bestDate)) {
                    bestDate = hols[i].date;
                    bestName = hols[i].name;
                }
            }
            if (bestName) {
                footer = "NEXT: " + String(bestDate % 100u) + " " +
                         MONTH_ABBR[(bestDate / 100u) % 100u] + " - " + bestName;
                footerColor = TFT_DARKGREY;
            }
        }
        tft.setTextFont(1);
        tft.setTextSize(1);
        if (footer.length() > 0) {
            tft.setTextDatum(TC_DATUM);
            tft.setTextColor(footerColor, clockBackgroundColor);
            tft.drawString(fitTextToWidth(tft, footer, sx(190)), screenWidth / 2, sy(230));
        }

        // Home zone's sunrise (left) and sunset (right) in the footer
        // corners. The sun/moon glyphs are the clear-sky weather icons.
        int riseMin, setMin;
        if (zoneSunTimes(worldZones[0], riseMin, setMin)) {
            drawWeatherIcon(tft, sx(14), sy(230) + 2, 0, false); // sun
            tft.setTextDatum(TL_DATUM);
            tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
            tft.drawString(formatMinutesOfDay(riseMin), sx(26), sy(230));

            drawWeatherIcon(tft, sx(306), sy(230) + 2, 0, true); // moon
            tft.setTextDatum(TR_DATUM);
            tft.drawString(formatMinutesOfDay(setMin), sx(296), sy(230));
        }
    }

    // Clock in the header, refreshed every minute
    bool pm;
    String hhmm = formatHHMM(local, pm);
    if (!SHOW_24HOUR) hhmm += pm ? " PM" : " AM";
    tft.fillRect(sx(196), 2, screenWidth - sx(196), 30, clockBackgroundColor);
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(getDayNightColor(worldZones[0]), clockBackgroundColor);
    tft.drawString(hhmm, screenWidth - 6, 4);

    // Home's current weather between the month name and the clock
    tft.fillRect(sx(138), 2, sx(58), 30, clockBackgroundColor);
    ZoneWeather w = getZoneWeather(0);
    if (projectConfig.quadWeather && w.valid) {
        drawWeatherIcon(tft, sx(148), 16, w.weatherCode, zoneIsNight(worldZones[0]));
        String temp = String(displayTemp(w.tempC));
        tft.setTextFont(2);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, clockBackgroundColor);
        tft.drawString(temp, sx(162), 8);
        tft.drawCircle(sx(162) + 2 + tft.textWidth(temp) + 2, 10, 2, TFT_WHITE);
    }
}

/*-------- Weather face ----------*/
// Current conditions for all four cities (Open-Meteo), each with local time.

static void renderWeatherFace(bool full)
{
    time_t local = worldZones[0].tz.now();
    bool big = largeFace();

    if (full) {
        tft.fillScreen(clockBackgroundColor);
        tft.drawFastHLine(sx(10), sy(58), sx(300), TFT_DARKGREY);
    }

    // Header: home time on the left, date + data age on the right
    tft.fillRect(0, 0, screenWidth, sy(56), clockBackgroundColor);
    bool pm;
    String hhmm = formatHHMM(local, pm);
    tft.setTextFont(4);
    tft.setTextSize(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(getDayNightColor(worldZones[0]), clockBackgroundColor);
    tft.drawString(hhmm, sx(8), 2);
    if (!SHOW_24HOUR) {
        int hhmmW = tft.textWidth(hhmm); // measured while font 4 size 2 is active
        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.drawString(pm ? "PM" : "AM", sx(8) + hhmmW + 8, 8);
    }

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString(String(DAY_NAMES[weekday(local)]) + " " + formatDate(local),
                   screenWidth - 8, 6);

    long age = weatherAgeMinutes();
    tft.setTextFont(1);
    tft.setTextColor(age < 0 ? TFT_RED : TFT_DARKGREY, clockBackgroundColor);
    tft.drawString(age < 0 ? "no weather data" : "updated " + String(age) + " min ago",
                   screenWidth - 8, 28);

    // One row per zone: name + local time (with humidity) on the left, the
    // condition icon over today's high/low (or a near-term precipitation
    // notice) in the middle, temp + condition (or an active weather alert)
    // on the right. The large screen bumps the name/high-low fonts a step.
    for (int i = 0; i < 4; i++) {
        int ry = sy(64 + i * 44);
        tft.fillRect(0, ry, screenWidth, sy(40), clockBackgroundColor);

        tft.setTextFont(big ? 4 : 2);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(getDayNightLabelColor(worldZones[i]), clockBackgroundColor);
        tft.drawString(worldZones[i].name, sx(8), ry);

        bool zonePm;
        String zoneTime = formatHHMM(worldZones[i].tz.now(), zonePm);
        if (!SHOW_24HOUR) zoneTime += zonePm ? " PM" : " AM";
        tft.setTextFont(2);
        tft.setTextColor(getDayNightColor(worldZones[i]), clockBackgroundColor);
        tft.drawString(zoneTime, sx(8), ry + (big ? 30 : 18));
        int timeW = tft.textWidth(zoneTime);

        ZoneWeather w = getZoneWeather(i);
        if (w.valid) {
            // Current relative humidity, small, after the local time
            if (w.humidity >= 0) {
                tft.setTextFont(1);
                tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
                tft.drawString(String(w.humidity) + "%", sx(8) + timeW + 8,
                               ry + (big ? 34 : 22));
            }

            // Temperature with a hand-drawn degree mark (the bitmap fonts have
            // no '°' glyph)
            tft.setTextFont(4);
            tft.setTextDatum(TR_DATUM);
            tft.setTextColor(TFT_WHITE, clockBackgroundColor);
            tft.drawString(String(displayTemp(w.tempC)), sx(284), ry);
            tft.drawCircle(sx(284) + 7, ry + 7, 3, TFT_WHITE);
            tft.setTextFont(2);
            tft.setTextDatum(TL_DATUM);
            tft.drawString(String(tempUnitLetter()), sx(284) + 13, ry + 2);

            // Right bottom: an active weather alert wins, else a near-term
            // precipitation notice ("RAIN IN 30M"), else the condition text.
            // Alert/notice use the small font so their longer strings fit.
            String alert = getZoneAlert(i);
            String notice = getZonePrecipNotice(i);
            tft.setTextDatum(TR_DATUM);
            if (alert.length() > 0) {
                tft.setTextFont(1);
                tft.setTextColor(WX_ALERT_COLOR, clockBackgroundColor);
                tft.drawString(fitTextToWidth(tft, alert, sx(130)),
                               screenWidth - 8, ry + (big ? 32 : 26));
            } else if (notice.length() > 0) {
                tft.setTextFont(1);
                tft.setTextColor(weatherNoticeColor(notice), clockBackgroundColor);
                tft.drawString(fitTextToWidth(tft, notice, sx(130)),
                               screenWidth - 8, ry + (big ? 32 : 26));
            } else {
                tft.setTextFont(2);
                tft.setTextColor(weatherCodeColor(w.weatherCode), clockBackgroundColor);
                tft.drawString(weatherCodeText(w.weatherCode),
                               screenWidth - 8, ry + (big ? 28 : 22));
            }

            // Middle: condition glyph (same night-aware icons as the quadrant
            // badges) over today's forecast high/low.
            drawWeatherIcon(tft, sx(158), ry + (big ? 14 : 10), w.weatherCode,
                            zoneIsNight(worldZones[i]));
            if (w.hasMinMax) {
                tft.setTextFont(1);
                tft.setTextSize(big ? 2 : 1);
                tft.setTextDatum(TC_DATUM);
                tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
                tft.drawString(String(displayTemp(w.tempMaxC)) + "/" +
                                   String(displayTemp(w.tempMinC)),
                               sx(158), ry + (big ? 32 : 28));
                tft.setTextSize(1);
            }
        } else {
            tft.setTextFont(2);
            tft.setTextDatum(TR_DATUM);
            tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
            tft.drawString("--", screenWidth - 8, ry + 8);
        }
    }
}

/*-------- Markets face ----------*/
// Every exchange the clock knows about, at a glance - independent of which
// cities occupy the four quadrants. One row per exchange: local time on the
// right, open/closed/countdown status below the name, colored like the
// quadrant status lines. The zones tick on the presets' built-in POSIX rules
// (correct incl. DST) so this face never needs the timezone server.

struct MarketDef
{
    const char *exchange;
    const char *city;
    const char *tz;
};

static const MarketDef MARKET_DEFS[] = {
    {"NYSE", "NEW YORK", "America/New_York"},
    {"LSE", "LONDON", "Europe/London"},
    {"SSE", "SHANGHAI", "Asia/Shanghai"},
    {"TSE", "TOKYO", "Asia/Tokyo"},
    {"HKEX", "HONG KONG", "Asia/Hong_Kong"},
};
static const int MARKET_DEF_COUNT = sizeof(MARKET_DEFS) / sizeof(MARKET_DEFS[0]);

static WorldClockZone marketZones[MARKET_DEF_COUNT];
static bool marketZonesReady = false;

static void initMarketZones()
{
    if (marketZonesReady) return;
    for (int i = 0; i < MARKET_DEF_COUNT; i++) {
        marketZones[i].name = MARKET_DEFS[i].city;
        marketZones[i].timezone = MARKET_DEFS[i].tz;
        marketZones[i].market = getMarketInfoForTimezone(MARKET_DEFS[i].tz);
        marketZones[i].tz.setPosix(getPosixFallback(MARKET_DEFS[i].tz));
    }
    marketZonesReady = true;
}

static void renderMarketsFace(bool full)
{
    initMarketZones();
    bool big = largeFace();

    if (full) {
        tft.fillScreen(clockBackgroundColor);
        tft.setTextFont(4);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, clockBackgroundColor);
        tft.drawString("MARKETS", sx(8), 4);
        tft.drawFastHLine(sx(10), sy(34), sx(300), TFT_DARKGREY);
    }

    // Home-zone date + clock in the header, refreshed every minute
    time_t homeLocal = worldZones[0].tz.now();
    bool pm;
    String hhmm = formatHHMM(homeLocal, pm);
    if (!SHOW_24HOUR) hhmm += pm ? " PM" : " AM";
    tft.fillRect(sx(126), 2, screenWidth - sx(126), 30, clockBackgroundColor);
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(getDayNightColor(worldZones[0]), clockBackgroundColor);
    tft.drawString(hhmm, screenWidth - 6, 4);

    char headerDate[8];
    if (NOT_US_DATE) {
        sprintf(headerDate, "%02d/%02d", day(homeLocal), month(homeLocal));
    } else {
        sprintf(headerDate, "%02d/%02d", month(homeLocal), day(homeLocal));
    }
    tft.setTextFont(2);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, clockBackgroundColor);
    tft.drawString(String(DAY_NAMES[weekday(homeLocal)]) + " " + headerDate, sx(170), 10);

    // One row per exchange: status dot + exchange/city and the colored status
    // line on the left, local time with its weekday on the right, and a
    // trading-day progress bar along the bottom during regular hours. The
    // large screen bumps the exchange/city/weekday fonts a step.
    for (int i = 0; i < MARKET_DEF_COUNT; i++) {
        int ry = sy(42 + i * 40);
        tft.fillRect(0, ry, screenWidth, sy(38), clockBackgroundColor);

        // Status first: its color also feeds the row's at-a-glance dot.
        // Recomputed each minute - transitions land on minute boundaries.
        String status = getMarketStatus(marketZones[i]);
        uint16_t statusColor = getMarketStatusColor(status);
        tft.fillCircle(sx(11), ry + (big ? 10 : 7), big ? 4 : 3, statusColor);

        tft.setTextFont(big ? 4 : 2);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, clockBackgroundColor);
        tft.drawString(MARKET_DEFS[i].exchange, sx(20), ry);

        tft.setTextFont(big ? 2 : 1);
        tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
        tft.drawString(MARKET_DEFS[i].city, sx(76) + (big ? 24 : 0), ry + (big ? 7 : 4));

        time_t zoneLocal = marketZones[i].tz.now();
        bool zonePm;
        String zoneTime = formatHHMM(zoneLocal, zonePm);
        if (!SHOW_24HOUR) zoneTime += zonePm ? "P" : "A";
        tft.setTextFont(4);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(getDayNightColor(marketZones[i]), clockBackgroundColor);
        tft.drawString(zoneTime, screenWidth - 8, ry);

        // The exchange's local weekday under its time: makes weekend closures
        // (and the day rolling over across the date line) self-explanatory.
        tft.setTextFont(big ? 2 : 1);
        tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
        tft.drawString(DAY_NAMES[weekday(zoneLocal)], screenWidth - 8,
                       ry + (big ? 30 : 28));

        tft.setTextFont(2);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(statusColor, clockBackgroundColor);
        tft.drawString(status, sx(20), ry + (big ? 28 : 18));

        // Trading-day progress while inside regular hours, like the quadrant
        // market progress bar.
        float frac;
        if (marketDayProgress(marketZones[i], frac)) {
            int barY = ry + (big ? 46 : 35);
            tft.fillRect(sx(20), barY, sx(200), big ? 3 : 2, 0x39E7 /* dim grey track */);
            tft.fillRect(sx(20), barY, (int)(sx(200) * frac + 0.5f), big ? 3 : 2, TFT_GREEN);
        }
    }
}

/*-------- Dispatcher ----------*/

void drawAlternateFace()
{
    static int lastFace = -1;
    static int lastMinute = -1;
    static int lastDay = -1;
    static uint32_t lastWeatherVersion = 0;
    static uint32_t lastHolidayVersion = 0;

    // The brightness bar overlay owns the screen; its timeout sets firstDraw
    // which triggers a full repaint here.
    if (brightnessBarVisible) return;

    time_t homeLocal = worldZones[0].tz.now();
    bool full = firstDraw || projectConfig.clockFace != lastFace;
    bool dayChanged = full || day(homeLocal) != lastDay;
    bool minuteTick = full || dayChanged || minute(homeLocal) != lastMinute;

    // Fresh data from the background weather task repaints the faces that
    // show weather (weather, big clock, calendar header) right away instead
    // of waiting for the next minute tick; likewise the calendar grid and the
    // big face's holiday line redraw when async holiday data lands.
    bool paintsWeather = projectConfig.clockFace == FACE_WEATHER ||
                         projectConfig.clockFace == FACE_BIG ||
                         projectConfig.clockFace == FACE_CALENDAR;
    bool paintsHolidays = projectConfig.clockFace == FACE_CALENDAR ||
                          projectConfig.clockFace == FACE_BIG;
    bool weatherChanged = paintsWeather && (weatherDataVersion() != lastWeatherVersion);
    bool holidaysChanged = paintsHolidays && (holidaysDataVersion() != lastHolidayVersion);

    if (!minuteTick && !weatherChanged && !holidaysChanged) return;

    switch (projectConfig.clockFace) {
    case FACE_BIG:
        renderBigClockFace(full);
        break;
    case FACE_CALENDAR:
        renderCalendarFace(dayChanged || holidaysChanged, homeLocal);
        break;
    case FACE_WEATHER:
        renderWeatherFace(full);
        break;
    case FACE_MARKETS:
        renderMarketsFace(full);
        break;
    default:
        break;
    }
    lastWeatherVersion = weatherDataVersion();
    lastHolidayVersion = holidaysDataVersion();

    lastFace = projectConfig.clockFace;
    lastMinute = minute(homeLocal);
    lastDay = day(homeLocal);
    firstDraw = false;
}
