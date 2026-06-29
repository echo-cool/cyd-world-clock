/*******************************************************************
    /* === CYD ROLLING CLOCK ===
    This example shows a digital clock with a rolling effect as the digits change.
    Most of the code are borrowed from other examples. Thanks Internet!


    Brian:
    Took the rolling clock example and added the following to make it webflashable

    - Wifi manager for configuring
    - Double reset detector for entering config mode
    - Saving and loading config
    - NTP and Timezones

    If you find what I do useful and would like to support me,
    please consider becoming a sponsor on Github
    https://github.com/sponsors/witnessmenow/

    Written by Brian Lough
    YouTube: https://www.youtube.com/brianlough
    Twitter: https://twitter.com/witnessmenow
 *******************************************************************/

#include "genericBaseProject.h"
#include "ClockLogic.h"

void setup()
{
    Serial.begin(115200);
    Serial.println("Main setup running on core " + String(xPortGetCoreID()));

    baseProjectSetup();
    // You will be fully connected by the time you are here

    rollingClockSetup(projectConfig.twentyFourHour, projectConfig.usDateFormat);
    Serial.println("Setup complete. Main loop will run on core " + String(xPortGetCoreID()));
}

void loop()
{
    baseProjectLoop();
    drawRollingClock();

    // Small pause to avoid busy-spinning the CPU. Touch debounce (50ms) and the
    // market-status flash (1s) are all coarser than this, so responsiveness is
    // unaffected while CPU load and heat are reduced.
    delay(20);
}