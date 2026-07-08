#ifndef DATE_MATH_H
#define DATE_MATH_H

// ---------------------------------------------------------------------------
// Proleptic Gregorian calendar math (Howard Hinnant's algorithms).
//
// Pure integer math with no Arduino / hardware dependencies, so it is compiled
// and unit-tested on the host (see test/test_datemath). The firmware uses it
// for day-of-week, weekend/holiday scans and cross-zone day-offset labels.
// ---------------------------------------------------------------------------

// Days since 1970-01-01 for a civil date; ((result + 4) % 7) gives the day of
// week with 0 = Sunday.
long daysFromCivil(int y, int m, int d);

// Inverse of daysFromCivil: civil date for a days-since-1970 count. Used to
// walk forward over real calendar dates (weekends, market holidays).
void civilFromDays(long days, int &y, int &m, int &d);

#endif // DATE_MATH_H
