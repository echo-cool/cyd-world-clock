#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

// Wipe every persisted setting and credential, returning the device to its
// first-boot state, then reboot. Clears:
//   - the WiFi stack's NVS: portal-saved credentials and the ezTime timezone
//     cache (see wifi-persistence notes - these live in NVS, not SPIFFS)
//   - SPIFFS: display/format settings (project_config.json), cached holiday
//     calendars, and the double-reset-detector flag
//
// Exposed three ways: the serial console (FACTORYRESET), the web API
// (POST /api/factory-reset) and a button on the settings page. This does NOT
// return - it calls ESP.restart() once the erase is done.
void factoryReset();

#endif // FACTORY_RESET_H
