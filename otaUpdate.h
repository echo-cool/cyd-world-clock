#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

// ---------------------------------------------------------------------------
// The device's web server (port 80) and over-the-air firmware updates.
//
// Web pages, at http://esp32worldclock.local/ (or the device IP from the
// System status page):
//   /            - settings page (timezones, clock face, formats, brightness)
//   /update      - firmware updater (upload a compiled .bin)
//   /api/status  - diagnostics as JSON
//
// Firmware updates also work over ArduinoOTA / espota - uncomment the espota
// block in platformio.ini, or use the Arduino IDE's network port
// "esp32worldclock". The min_spiffs partition scheme provides the two OTA
// app slots.
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
