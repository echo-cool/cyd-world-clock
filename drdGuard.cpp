#include "drdGuard.h"

#include <Arduino.h>

// Must be defined before the library include - selects SPIFFS (not EEPROM)
// as the double-reset flag storage. Keep this file the only place the
// library is included so the storage choice can never diverge between
// translation units.
#define ESP_DRD_USE_SPIFFS true
#include <ESP_DoubleResetDetector.h>

// Number of seconds after reset during which a subsequent reset will be
// considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

static DoubleResetDetector *drd = nullptr;

bool drdSetupDetect()
{
    if (!drd) drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);
    return drd->detectDoubleReset();
}

void drdLoop()
{
    if (drd) drd->loop();
}

[[noreturn]] void rebootCleanly(unsigned long delayMs)
{
    if (drd) drd->stop(); // avoid the reboot registering as a double reset
    if (delayMs) delay(delayMs);
    ESP.restart();
    for (;;) {} // unreachable - ESP.restart() does not return
}
