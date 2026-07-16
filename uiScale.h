#ifndef UI_SCALE_H
#define UI_SCALE_H

// Every layout in this project is designed in 320x240 coordinates (the
// original CYD panel) and scaled to the actual panel at draw time - identity
// on the CYD, 1.5x/1.33x on the Hosyond 4.0" 480x320. This is the one shared
// copy of that mapping (round-half-up): a drifting duplicate would break
// layout only on the 480x320 board, which is easy to miss when testing on a
// CYD.

// Panel size in pixels (defined in ClockLogic.cpp, set from the display at
// boot). Declared here directly so this header stays free of heavy includes.
extern int screenWidth;
extern int screenHeight;

constexpr int UI_DESIGN_WIDTH = 320;
constexpr int UI_DESIGN_HEIGHT = 240;

// One design dimension scaled to an actual panel dimension, rounding half up.
inline int scaleDim(int value, int actual, int designBase)
{
    return (value * actual + designBase / 2) / designBase;
}

inline int scaleUiX(int value)
{
    return scaleDim(value, screenWidth, UI_DESIGN_WIDTH);
}

inline int scaleUiY(int value)
{
    return scaleDim(value, screenHeight, UI_DESIGN_HEIGHT);
}

#endif // UI_SCALE_H
