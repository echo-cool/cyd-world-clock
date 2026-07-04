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

/*-------- Big clock face ----------*/
// Home zone in 75px digits, with date, market status and a mini strip of the
// other three zones along the bottom.

static void renderBigClockFace(bool full)
{
    time_t local = worldZones[0].tz.now();
    uint16_t timeColor = getDayNightColor(worldZones[0]);

    if (full) {
        tft.fillScreen(clockBackgroundColor);

        tft.setTextFont(4);
        tft.setTextSize(1);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_WHITE, clockBackgroundColor);
        tft.drawString(worldZones[0].name, 160, 6);

        tft.drawFastHLine(10, 196, 300, TFT_DARKGREY);
    }

    // Big HH:MM (Font 8 carries only digits ':' '-' '.', so AM/PM is drawn
    // separately in a text font)
    bool pm;
    String hhmm = formatHHMM(local, pm);
    tft.fillRect(0, 44, 320, 80, clockBackgroundColor);
    tft.setTextFont(8);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(timeColor, clockBackgroundColor);
    tft.drawString(hhmm, 160, 46);
    int timeWidth = tft.textWidth(hhmm); // measured while font 8 is active

    if (!SHOW_24HOUR) {
        tft.setTextFont(2);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(pm ? "PM" : "AM", 160 + timeWidth / 2 + 6, 48);
    }

    // Day + date
    tft.fillRect(0, 130, 320, 28, clockBackgroundColor);
    tft.setTextFont(4);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(getDayNightLabelColor(worldZones[0]), clockBackgroundColor);
    tft.drawString(String(DAY_NAMES[weekday(local)]) + " " + formatDate(local), 160, 132);

    // Market status of the home zone (computed fresh - the once-per-minute
    // cache in hasTimeChanged only runs while the quad face is active)
    tft.fillRect(0, 164, 320, 20, clockBackgroundColor);
    if (worldZones[0].market.hasMarket) {
        String status = getMarketStatus(worldZones[0]);
        tft.setTextFont(2);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(getMarketStatusColor(status), clockBackgroundColor);
        tft.drawString(status, 160, 166);
    }

    // Mini strip: the other three zones
    tft.fillRect(0, 202, 320, 38, clockBackgroundColor);
    for (int k = 1; k < 4; k++) {
        int cx = 60 + (k - 1) * 100;
        String name = worldZones[k].name;
        if (name.length() > 12) name = name.substring(0, 12);

        tft.setTextFont(1);
        tft.setTextSize(1);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(getDayNightLabelColor(worldZones[k]), clockBackgroundColor);
        tft.drawString(name, cx, 204);

        bool zonePm;
        String zoneTime = formatHHMM(worldZones[k].tz.now(), zonePm);
        if (!SHOW_24HOUR) zoneTime += zonePm ? "P" : "A";
        tft.setTextFont(2);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(getDayNightColor(worldZones[k]), clockBackgroundColor);
        tft.drawString(zoneTime, cx, 216);
    }
}

/*-------- Calendar face ----------*/
// Month grid for the home zone with today highlighted; clock in the header.
// Public holidays are marked in gold, with today's / the next holiday's name
// in a footer line.

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
        tft.drawString(String(MONTH_ABBR[mo]) + " " + String(yr), 8, 4);

        static const char *WD[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
        tft.setTextFont(1);
        tft.setTextSize(1);
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(TFT_CYAN, clockBackgroundColor);
        for (int c = 0; c < 7; c++) {
            tft.drawString(WD[c], 6 + c * 44 + 22, 36);
        }

        // 1970-01-01 (daysFromCivil == 0) was a Thursday, so +4 lands the
        // week on 0 = Sunday.
        int firstDow = (int)((daysFromCivil(yr, mo, 1) + 4) % 7);
        int dim = daysInMonth(yr, mo);

        tft.setTextFont(1);
        tft.setTextSize(2);
        tft.setTextDatum(TC_DATUM);
        for (int d = 1; d <= dim; d++) {
            int cell = firstDow + d - 1;
            int col = cell % 7;
            int row = cell / 7;
            int cx = 6 + col * 44 + 22;
            int cy = 48 + row * 30;
            uint32_t ymd = (uint32_t)yr * 10000u + (uint32_t)mo * 100u + (uint32_t)d;
            bool holiday = holidayInList(hols, holCount, ymd) != nullptr;
            if (d == dd) {
                uint16_t boxColor = holiday ? TFT_GOLD : TFT_ORANGE;
                tft.fillRoundRect(6 + col * 44 + 4, cy - 3, 36, 22, 5, boxColor);
                tft.setTextColor(TFT_BLACK, boxColor);
            } else if (holiday) {
                tft.setTextColor(TFT_GOLD, clockBackgroundColor);
            } else {
                bool weekend = (col == 0 || col == 6);
                tft.setTextColor(weekend ? TFT_DARKGREY : TFT_WHITE, clockBackgroundColor);
            }
            tft.drawString(String(d), cx, cy);
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
        if (footer.length() > 0) {
            tft.setTextFont(1);
            tft.setTextSize(1);
            tft.setTextDatum(TC_DATUM);
            tft.setTextColor(footerColor, clockBackgroundColor);
            tft.drawString(footer, 160, 230);
        }
    }

    // Clock in the header, refreshed every minute
    bool pm;
    String hhmm = formatHHMM(local, pm);
    if (!SHOW_24HOUR) hhmm += pm ? " PM" : " AM";
    tft.fillRect(184, 2, 136, 30, clockBackgroundColor);
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(getDayNightColor(worldZones[0]), clockBackgroundColor);
    tft.drawString(hhmm, 314, 4);
}

/*-------- Weather face ----------*/
// Current conditions for all four cities (Open-Meteo), each with local time.

static void renderWeatherFace(bool full)
{
    time_t local = worldZones[0].tz.now();

    if (full) {
        tft.fillScreen(clockBackgroundColor);
        tft.drawFastHLine(10, 58, 300, TFT_DARKGREY);
    }

    // Header: home time on the left, date + data age on the right
    tft.fillRect(0, 0, 320, 56, clockBackgroundColor);
    bool pm;
    String hhmm = formatHHMM(local, pm);
    tft.setTextFont(4);
    tft.setTextSize(2);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(getDayNightColor(worldZones[0]), clockBackgroundColor);
    tft.drawString(hhmm, 8, 2);
    if (!SHOW_24HOUR) {
        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.drawString(pm ? "PM" : "AM", 148, 8);
    }

    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_WHITE, clockBackgroundColor);
    tft.drawString(String(DAY_NAMES[weekday(local)]) + " " + formatDate(local), 312, 6);

    long age = weatherAgeMinutes();
    tft.setTextFont(1);
    tft.setTextColor(age < 0 ? TFT_RED : TFT_DARKGREY, clockBackgroundColor);
    tft.drawString(age < 0 ? "no weather data" : "updated " + String(age) + " min ago", 312, 28);

    // One row per zone: name + local time on the left, temp + condition right
    for (int i = 0; i < 4; i++) {
        int ry = 64 + i * 44;
        tft.fillRect(0, ry, 320, 40, clockBackgroundColor);

        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(getDayNightLabelColor(worldZones[i]), clockBackgroundColor);
        tft.drawString(worldZones[i].name, 8, ry);

        bool zonePm;
        String zoneTime = formatHHMM(worldZones[i].tz.now(), zonePm);
        if (!SHOW_24HOUR) zoneTime += zonePm ? " PM" : " AM";
        tft.setTextColor(getDayNightColor(worldZones[i]), clockBackgroundColor);
        tft.drawString(zoneTime, 8, ry + 18);

        ZoneWeather w = getZoneWeather(i);
        if (w.valid) {
            // Temperature with a hand-drawn degree mark (the bitmap fonts have
            // no '°' glyph)
            tft.setTextFont(4);
            tft.setTextDatum(TR_DATUM);
            tft.setTextColor(TFT_WHITE, clockBackgroundColor);
            tft.drawString(String((int)lroundf(w.tempC)), 284, ry);
            tft.drawCircle(291, ry + 7, 3, TFT_WHITE);
            tft.setTextFont(2);
            tft.setTextDatum(TL_DATUM);
            tft.drawString("C", 297, ry + 2);

            tft.setTextFont(2);
            tft.setTextDatum(TR_DATUM);
            tft.setTextColor(weatherCodeColor(w.weatherCode), clockBackgroundColor);
            tft.drawString(weatherCodeText(w.weatherCode), 312, ry + 22);
        } else {
            tft.setTextFont(2);
            tft.setTextDatum(TR_DATUM);
            tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
            tft.drawString("--", 312, ry + 8);
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

    if (full) {
        tft.fillScreen(clockBackgroundColor);
        tft.setTextFont(4);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, clockBackgroundColor);
        tft.drawString("MARKETS", 8, 4);
        tft.drawFastHLine(10, 34, 300, TFT_DARKGREY);
    }

    // Home-zone clock in the header, refreshed every minute
    bool pm;
    String hhmm = formatHHMM(worldZones[0].tz.now(), pm);
    if (!SHOW_24HOUR) hhmm += pm ? " PM" : " AM";
    tft.fillRect(160, 2, 160, 30, clockBackgroundColor);
    tft.setTextFont(4);
    tft.setTextSize(1);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(getDayNightColor(worldZones[0]), clockBackgroundColor);
    tft.drawString(hhmm, 314, 4);

    // One row per exchange
    for (int i = 0; i < MARKET_DEF_COUNT; i++) {
        int ry = 42 + i * 40;
        tft.fillRect(0, ry, 320, 38, clockBackgroundColor);

        tft.setTextFont(2);
        tft.setTextSize(1);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, clockBackgroundColor);
        tft.drawString(MARKET_DEFS[i].exchange, 8, ry);

        tft.setTextFont(1);
        tft.setTextColor(TFT_DARKGREY, clockBackgroundColor);
        tft.drawString(MARKET_DEFS[i].city, 64, ry + 4);

        bool zonePm;
        String zoneTime = formatHHMM(marketZones[i].tz.now(), zonePm);
        if (!SHOW_24HOUR) zoneTime += zonePm ? "P" : "A";
        tft.setTextFont(4);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(getDayNightColor(marketZones[i]), clockBackgroundColor);
        tft.drawString(zoneTime, 312, ry);

        // Status recomputed each minute - transitions land on minute boundaries
        String status = getMarketStatus(marketZones[i]);
        tft.setTextFont(2);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(getMarketStatusColor(status), clockBackgroundColor);
        tft.drawString(status, 8, ry + 18);
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

    // Fresh data from the background weather task repaints the weather face
    // right away instead of waiting for the next minute tick; likewise the
    // calendar face redraws its grid when async holiday data lands.
    bool weatherChanged = (projectConfig.clockFace == FACE_WEATHER) &&
                          (weatherDataVersion() != lastWeatherVersion);
    bool holidaysChanged = (projectConfig.clockFace == FACE_CALENDAR) &&
                           (holidaysDataVersion() != lastHolidayVersion);

    if (!minuteTick && !weatherChanged && !holidaysChanged) return;

    switch (projectConfig.clockFace) {
    case FACE_BIG:
        renderBigClockFace(full);
        break;
    case FACE_CALENDAR:
        renderCalendarFace(dayChanged || holidaysChanged, homeLocal);
        lastHolidayVersion = holidaysDataVersion();
        break;
    case FACE_WEATHER:
        renderWeatherFace(full);
        lastWeatherVersion = weatherDataVersion();
        break;
    case FACE_MARKETS:
        renderMarketsFace(full);
        break;
    default:
        break;
    }

    lastFace = projectConfig.clockFace;
    lastMinute = minute(homeLocal);
    lastDay = day(homeLocal);
    firstDraw = false;
}
