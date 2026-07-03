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