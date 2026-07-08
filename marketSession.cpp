#include "marketSession.h"

namespace market
{
static const int DAY_MINUTES = 24 * 60;

bool inSession(int nowMin, int startMin, int endMin)
{
    if (endMin < startMin)
    {
        // Spans midnight (e.g. 20:00 -> 04:00): in session on either side.
        return (nowMin >= startMin || nowMin < endMin);
    }
    return (nowMin >= startMin && nowMin < endMin);
}

int minutesUntilClose(int nowMin, int startMin, int endMin)
{
    if (endMin < startMin && nowMin >= startMin)
    {
        // Pre-midnight half of a midnight-spanning session: close is tomorrow.
        return DAY_MINUTES - nowMin + endMin;
    }
    return endMin - nowMin;
}

int minutesUntilOpen(int nowMin, int startMin, int endMin)
{
    if (nowMin < startMin)
    {
        return startMin - nowMin;
    }
    if (endMin < startMin)
    {
        // Midnight-spanning window whose open is earlier today: next open wraps
        // into tomorrow.
        return DAY_MINUTES - nowMin + startMin;
    }
    // Normal same-day window already opened/closed: nothing more opens today
    // from this session.
    return -1;
}
} // namespace market
