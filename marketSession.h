#ifndef MARKET_SESSION_H
#define MARKET_SESSION_H

// ---------------------------------------------------------------------------
// Trading-session timing math, split out of ClockLogic's getMarketStatus.
//
// All times are minutes from local midnight (0..1439). A session whose close
// is numerically before its open (endMin < startMin) spans midnight, e.g. an
// overnight future from 20:00 (1200) to 04:00 (240). This midnight-wrap logic
// is exactly where an off-by-one hides, so it lives here dependency-free and
// is unit-tested on the host (test/test_marketsession).
// ---------------------------------------------------------------------------

namespace market
{
// Is nowMin within [startMin, endMin), accounting for a midnight-spanning
// window?
bool inSession(int nowMin, int startMin, int endMin);

// Minutes until the window closes, assuming nowMin is inside it (inSession
// true). Handles being in the pre-midnight half of a spanning session.
int minutesUntilClose(int nowMin, int startMin, int endMin);

// Minutes until the window next opens from nowMin. Returns a negative value
// for a normal same-day window whose open is already past (the caller then
// looks to a later day) - a midnight-spanning window instead wraps to its
// next open.
int minutesUntilOpen(int nowMin, int startMin, int endMin);
} // namespace market

#endif // MARKET_SESSION_H
