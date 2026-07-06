#include "genericBaseProject.h"
#include "ClockLogic.h"
#include "uiPages.h" // bootUiSettingsRequested - boot-time Settings shortcut

void setup()
{
    Serial.begin(115200);
    Log.println("Main setup running on core " + String(xPortGetCoreID()));

    baseProjectSetup();
    // You will be fully connected by the time you are here (unless the boot
    // Settings button cut the network waits short)

    rollingClockSetup(projectConfig.twentyFourHour, projectConfig.usDateFormat);

    // A Settings tap on the init screen skips straight to the settings page,
    // where the WiFi login helper, status and logs are one tap away.
    if (bootUiSettingsRequested())
    {
        switchToScreen(SCREEN_SETTINGS);
    }

    Log.println("Setup complete. Main loop will run on core " + String(xPortGetCoreID()));
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