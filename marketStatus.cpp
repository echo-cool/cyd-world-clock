/*-------- Market / trading-session status ----------*/
// Moved verbatim out of ClockLogic.cpp: the market status line, its display
// color and the regular-session progress/close queries.

#include "marketStatus.h"

#include <TFT_eSPI.h> // status colors (TFT_GREEN, TFT_RED, ...)

#include "dateMath.h"      // daysFromCivil / civilFromDays (host-tested pure math)
#include "marketHolidays.h"
#include "marketSession.h" // host-tested trading-session membership math

// Days since 1970-01-01 for a given civil date (Howard Hinnant's algorithm).
// Used to compare calendar dates between timezones robustly across month/year
// boundaries instead of comparing bare day-of-month numbers.
// Show the countdown to the next open only when the open is less than a day
// away. Longer waits used to render as "OPENS IN 2D 8H", which reads too much
// like a time ("20 8H") at a glance - those now show a plain red "CLOSED".
static const long MARKET_COUNTDOWN_MAX_MIN = 24 * 60;

// Format a sub-24h countdown as "5H 03M" or "45 MIN" depending on magnitude.
static String formatOpenCountdown(long minutes)
{
    char buf[12];
    if (minutes >= 60) {
        sprintf(buf, "%ldH %02ldM", minutes / 60, minutes % 60);
    } else {
        sprintf(buf, "%ld MIN", minutes);
    }
    return String(buf);
}

// Minutes until the next REGULAR session opens, walking real calendar dates
// so weekends AND full-day exchange holidays (marketHolidays.cpp) are skipped.
// The scan covers 30 days so long closures (SSE Spring Festival / golden
// week) still resolve. DST shifts between now and the open are ignored (at
// most an hour off around a transition weekend); half-day early closes don't
// move the opens, so they don't matter here.
static long minutesToNextRegularOpen(const WorldClockZone &zone, int y, int mo, int dd, int totalMinutes)
{
    long today = daysFromCivil(y, mo, dd);
    for (int d = 0; d <= 30; d++) {
        long days = today + d;
        int wd = (int)((days + 4) % 7) + 1; // 1=Sunday ... 7=Saturday
        if (wd == 1 || wd == 7) continue;   // markets closed on weekends
        int fy, fm, fd;
        civilFromDays(days, fy, fm, fd);
        if (isMarketHoliday(zone.market.exchange, fy, fm, fd)) continue;
        long best = -1;
        for (int i = 0; i < zone.market.sessionCount; i++) {
            const TradingSession &session = zone.market.sessions[i];
            if (session.sessionName != "REGULAR") continue;
            int start = session.openHour * 60 + session.openMinute;
            if (d == 0 && start <= totalMinutes) continue; // already past today
            long m = d * 1440L + start - totalMinutes;
            if (best < 0 || m < best) best = m;
        }
        if (best >= 0) return best;
    }
    return -1;
}

// Status line for a market that is closed: when the next regular open is
// under 24 hours away, count down to it; further out (long holidays,
// weekends viewed early) show a plain "CLOSED". Worded "OPENS IN" (not
// "OPEN IN") so shouldMessageFlash() doesn't treat this always-on countdown
// as one of the <=10 minute flashing alerts.
static String closedStatusWithCountdown(const WorldClockZone &zone, int y, int mo, int dd, int totalMinutes)
{
    long m = minutesToNextRegularOpen(zone, y, mo, dd, totalMinutes);
    if (m < 0 || m >= MARKET_COUNTDOWN_MAX_MIN) return zone.market.exchange + " CLOSED";
    return zone.market.exchange + " OPENS IN " + formatOpenCountdown(m);
}

String getMarketStatus(WorldClockZone &zone)
{
    if (!zone.market.hasMarket) {
        return ""; // No market for this zone
    }

    time_t local = zone.tz.now();
    int currentHour = hour(local);
    int currentMinute = minute(local);
    int currentDayOfWeek = weekday(local); // 1=Sunday, 2=Monday, ..., 7=Saturday
    int currentTotalMinutes = currentHour * 60 + currentMinute;
    int currentYear = year(local);
    int currentMonth = month(local);
    int currentDayOfMonth = day(local);

    // Full-day exchange holiday: closed all day, countdown to the next open
    // (the countdown itself skips holidays too).
    if (isMarketHoliday(zone.market.exchange, currentYear, currentMonth, currentDayOfMonth)) {
        return closedStatusWithCountdown(zone, currentYear, currentMonth, currentDayOfMonth, currentTotalMinutes);
    }

    // Half-day early close (e.g. NYSE Black Friday 1 PM): the session loop
    // below truncates sessions at this time and skips the ones that would
    // start after it. -1 on a normal day.
    int earlyCloseMinutes = marketEarlyCloseMinutes(zone.market.exchange, currentYear,
                                                    currentMonth, currentDayOfMonth);

    // Weekend logic - NYSE is closed on Saturday and Sunday
    if (currentDayOfWeek == 7 || currentDayOfWeek == 1) { // Saturday or Sunday
        // The overnight session (Blue Ocean ATS) starts its trading week on
        // Sunday evening at the open time from the session table (8 PM ET)
        // and runs into Monday 4 AM; the rest of the weekend is closed. Its
        // Monday-morning close is hours away all Sunday evening, so no
        // "CLOSE IN N MIN" case is needed here.
        if (currentDayOfWeek == 1 && zone.market.exchange == "NYSE") {
            for (int i = 0; i < zone.market.sessionCount; i++) {
                const TradingSession &session = zone.market.sessions[i];
                if (session.sessionName != "OVERNIGHT") continue;
                int sundayStart = session.openHour * 60 + session.openMinute;
                if (currentTotalMinutes >= sundayStart) {
                    return zone.market.exchange + " " + session.sessionName + " OPEN";
                }
            }
        }
        return closedStatusWithCountdown(zone, currentYear, currentMonth, currentDayOfMonth, currentTotalMinutes);
    }

    // Check each trading session
    for (int i = 0; i < zone.market.sessionCount; i++) {
        const TradingSession &session = zone.market.sessions[i];
        if (session.sessionName.length() == 0) continue; // Skip empty sessions

        int sessionStart = session.openHour * 60 + session.openMinute;
        int sessionEnd = session.closeHour * 60 + session.closeMinute;

        // The overnight session runs Sunday through Thursday nights only:
        // there is no Friday-evening start (the trading week ends Friday
        // 4 AM). Before the close time we are in Thursday night's tail,
        // which does run; past it, skip the session so neither the evening
        // "OPEN" nor its "OPEN IN N MIN" countdown can show on a Friday.
        if (session.sessionName == "OVERNIGHT" && currentDayOfWeek == 6 &&
            currentTotalMinutes >= sessionEnd) {
            continue;
        }

        // On a half day the exchange stops at earlyCloseMinutes: sessions
        // that would start at/after it don't run, the one in progress ends
        // there instead.
        if (earlyCloseMinutes >= 0) {
            if (sessionEnd < sessionStart) {
                // Spans midnight: tonight's session won't start. Keep only the
                // after-midnight tail, which belongs to the previous
                // (full-length) trading day.
                if (currentTotalMinutes >= sessionEnd) continue;
            } else {
                if (sessionStart >= earlyCloseMinutes) continue;
                if (sessionEnd > earlyCloseMinutes) sessionEnd = earlyCloseMinutes;
            }
        }

        // Membership + countdown math (incl. the midnight-spanning cases)
        // lives in market::* so it is unit-tested on the host.
        if (market::inSession(currentTotalMinutes, sessionStart, sessionEnd)) {
            // Currently in this session - check if closing soon
            int minutesToClose = market::minutesUntilClose(currentTotalMinutes, sessionStart, sessionEnd);

            if (minutesToClose <= MARKET_STATUS_MESSAGE_MIN) {
                if (session.sessionName == "REGULAR") {
                    return zone.market.exchange + " CLOSE IN " + String(minutesToClose) + " MIN";
                } else {
                    return zone.market.exchange + " " + session.sessionName + " CLOSE IN " + String(minutesToClose) + " MIN";
                }
            }

            if (session.sessionName == "REGULAR") {
                return zone.market.exchange + " OPEN";
            } else {
                return zone.market.exchange + " " + session.sessionName + " OPEN";
            }
        }

        // Check if next session is opening soon. A negative result means a
        // normal same-day session whose open is already past - look to the
        // next day (handled by later sessions / the closed-countdown below).
        int minutesToOpen = market::minutesUntilOpen(currentTotalMinutes, sessionStart, sessionEnd);
        if (minutesToOpen < 0) {
            continue;
        }

        if (minutesToOpen <= MARKET_STATUS_MESSAGE_MIN) {
            if (session.sessionName == "REGULAR") {
                return zone.market.exchange + " OPEN IN " + String(minutesToOpen) + " MIN";
            } else {
                return zone.market.exchange + " " + session.sessionName + " OPEN IN " + String(minutesToOpen) + " MIN";
            }
        }
    }

    return closedStatusWithCountdown(zone, currentYear, currentMonth, currentDayOfMonth, currentTotalMinutes);
}

uint16_t getMarketStatusColor(String status)
{
    // Countdown to the next regular open ("NYSE OPENS IN 5H 03M") - checked
    // first because it would otherwise match the generic " OPEN" case below.
    // Yellow: "about to open" sits between closed (red) and open (green).
    if (status.indexOf("OPENS IN") != -1) {
        return TFT_YELLOW;
    }

    // Flashing "about to open" countdown, any session ("NYSE PRE-MARKET OPEN
    // IN 5 MIN") - must also beat the session-type checks below, which would
    // otherwise match their "... OPEN" substring and show the steady
    // in-session color for a session that hasn't started yet.
    if (status.indexOf("OPEN IN") != -1) {
        return TFT_YELLOW;
    }

    // Check for specific session types first (before generic OPEN check)
    if (status.indexOf("AFTER-HRS OPEN") != -1) {
        return TFT_CYAN;    // Cyan for extended/after-hours trading
    } else if (status.indexOf("OVERNIGHT OPEN") != -1 || status.indexOf("PRE-MARKET OPEN") != -1) {
        return TFT_BLUE;    // Blue for overnight/pre-market
    } else if (status.indexOf("CLOSING OPEN") != -1) {
        return TFT_YELLOW;  // Yellow for closing auction period
    } else if (status.indexOf("CLOSED") != -1) {
        return TFT_RED;     // Red for closed market
    } else if (status.indexOf(" OPEN") != -1 && status.indexOf("OPEN ") == -1) {
        return TFT_GREEN;   // Bright green for regular trading (e.g. "NYSE OPEN")
    } else if (status.indexOf("OPEN ") != -1) {
        return TFT_YELLOW;  // Yellow for opening soon countdown (e.g. "NYSE OPEN 15MIN")
    } else if (status.indexOf("CLOSE ") != -1) {
        return TFT_ORANGE;  // Orange for closing soon countdown (e.g. "NYSE CLOSE 10MIN")
    } else {
        return TFT_WHITE;   // Default color
    }
}

bool shouldMessageFlash(String message)
{
    return (message.indexOf("CLOSE IN") != -1 || message.indexOf("OPEN IN") != -1);
}

// Minutes until the REGULAR session currently in progress closes; -1 when
// no regular session is running. Half-day early closes truncate the session,
// matching getMarketStatus. Feeds the markets face's "CLOSES IN" detail -
// the quadrants keep the shorter "OPEN" status line.
long marketMinutesToRegularClose(WorldClockZone &zone)
{
    if (!zone.market.hasMarket) return -1;

    time_t local = zone.tz.now();
    int wd = weekday(local);
    if (wd == 1 || wd == 7) return -1; // weekend
    if (isMarketHoliday(zone.market.exchange, year(local), month(local), day(local))) return -1;

    int early = marketEarlyCloseMinutes(zone.market.exchange, year(local), month(local), day(local));
    int nowMin = hour(local) * 60 + minute(local);
    for (int i = 0; i < zone.market.sessionCount; i++) {
        const TradingSession &s = zone.market.sessions[i];
        if (s.sessionName != "REGULAR") continue;
        int start = s.openHour * 60 + s.openMinute;
        int end = s.closeHour * 60 + s.closeMinute;
        if (early >= 0) {
            if (start >= early) continue;
            if (end > early) end = early;
        }
        if (market::inSession(nowMin, start, end)) {
            return market::minutesUntilClose(nowMin, start, end);
        }
    }
    return -1;
}

// Fraction (0..1) of the exchange's regular trading day already elapsed;
// false when the exchange is outside it (weekend, holiday, before open /
// after close). The span runs from the first REGULAR open to the last
// REGULAR close - the SSE lunch break stays inside the span - and half-day
// early closes truncate it, matching getMarketStatus. Shared with the
// markets face (clockFaces.cpp).
bool marketDayProgress(WorldClockZone &zone, float &frac)
{
    if (!zone.market.hasMarket) return false;

    time_t local = zone.tz.now();
    int wd = weekday(local);
    if (wd == 1 || wd == 7) return false; // weekend
    if (isMarketHoliday(zone.market.exchange, year(local), month(local), day(local))) return false;

    int openMin = -1, closeMin = -1;
    for (int i = 0; i < zone.market.sessionCount; i++) {
        const TradingSession &s = zone.market.sessions[i];
        if (s.sessionName != "REGULAR") continue;
        int start = s.openHour * 60 + s.openMinute;
        int end = s.closeHour * 60 + s.closeMinute;
        if (openMin < 0 || start < openMin) openMin = start;
        if (end > closeMin) closeMin = end;
    }
    if (openMin < 0) return false;

    int early = marketEarlyCloseMinutes(zone.market.exchange, year(local), month(local), day(local));
    if (early >= 0 && early < closeMin) closeMin = early;

    int nowMin = hour(local) * 60 + minute(local);
    if (closeMin <= openMin || nowMin < openMin || nowMin >= closeMin) return false;
    frac = (float)(nowMin - openMin) / (float)(closeMin - openMin);
    return true;
}
