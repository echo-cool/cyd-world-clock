#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

// ---------------------------------------------------------------------------
// The device's web server (port 80) and over-the-air firmware updates.
//
// Web pages, at http://esp32worldclock.local/ (hostname configurable on the
// settings page; or use the device IP from the System status page):
//   /            - settings page (timezones, face, formats, brightness,
//                  night dimming, hostname, config backup/restore)
//   /update      - firmware updater (upload a compiled .bin)
//   /logs        - recent log lines (auto-refreshing viewer)
//   /api/status  - diagnostics as JSON
//   /api/countdown - start, pause, resume or reset the countdown (POST)
//   /api/config  - settings backup as JSON (GET) / restore (POST the same
//                  JSON back; the device saves it and reboots to apply)
//   /api/logs    - the log tail as plain text
//
// Firmware updates also work over ArduinoOTA / espota - uncomment the espota
// block in platformio.ini, or use the Arduino IDE's network port named after
// the configured hostname. The min_spiffs partition scheme provides the two
// OTA app slots.
//
// Set OTA_PASSWORD in secrets.h to require authentication everywhere
// (HTTP Basic auth on the web pages, username "admin").
// ---------------------------------------------------------------------------

// True while an update is being received; background work (the weather fetch
// task) pauses so the flash writes get the radio and CPU to themselves.
extern volatile bool otaInProgress;

void setupOTA();  // call once, after WiFi is connected
void handleOTA(); // call every loop iteration

#endif // OTA_UPDATE_H
