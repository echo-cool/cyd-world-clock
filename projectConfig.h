#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

#include <Arduino.h>

#define PROJECT_CONFIG_JSON "/project_config.json"

#define PROJECT_TIME_ZONE_LABEL "timeZone"
#define PROJECT_TIME_TWENTY_FOUR_HOUR "twentyFourHour"
#define PROJECT_TIME_US_DATE "usDate"
#define PROJECT_ZONE_NAME_PREFIX "zoneName"
#define PROJECT_ZONE_TZ_PREFIX "zoneTz"
#define PROJECT_BRIGHTNESS "brightness"
#define PROJECT_CLOCK_FACE "clockFace"
#define PROJECT_HOSTNAME "hostname"
#define PROJECT_NIGHT_START "nightStartHour"
#define PROJECT_NIGHT_END "nightEndHour"
#define PROJECT_NIGHT_BRIGHTNESS "nightBrightness"

// Lowercase, keep only [a-z0-9-], trim edge dashes, cap at 32 chars; falls
// back to "esp32worldclock" when nothing usable is left. Applied to every
// hostname that enters the config (web settings, /api/config import, SPIFFS).
String sanitizeHostname(const String &raw);

class ProjectConfig
{
public:
  // https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
  String timeZone = "America/Los_Angeles"; // seems to be something wrong with Europe/Dublin

  bool twentyFourHour = false;

  bool usDateFormat = false;

  // Per-quadrant world clock timezones, selectable from the touch UI.
  // Defaults match the compiled-in worldZones in ClockLogic.cpp.
  String zoneName[4] = {"SANTA CLARA", "NEW YORK", "BEIJING", "LONDON"};
  String zoneTZ[4] = {"America/Los_Angeles", "America/New_York",
                      "Asia/Shanghai", "Europe/London"};

  // User's preferred backlight level (1-255). Restored on boot and used as the
  // daytime target by auto-brightness.
  int brightness = 80;

  // Home-screen face (ClockFace enum in clockFaces.h): 0 = quad world clock,
  // 1 = big clock, 2 = calendar, 3 = weather, 4 = markets.
  int clockFace = 0;

  // mDNS / OTA hostname ("<hostname>.local"). Changeable from the web
  // settings page so two clocks on one network don't collide; applied on the
  // next boot (mDNS registers during setup).
  String hostname = "esp32worldclock";

  // Night dimming, used by auto-brightness (ClockLogic.cpp):
  //  - nightBrightness (1-255) is the backlight target in a dark room (light
  //    sensor) or inside the schedule window below (sensor fallback).
  //  - The window is in home-zone hours; start == end disables the schedule
  //    (the light sensor, when trusted, works regardless).
  int nightStartHour = 1;
  int nightEndHour = 7;
  int nightBrightness = 1;

  bool fetchConfigFile();
  bool saveConfigFile();

  // The current settings as pretty-printed JSON (the /api/config backup).
  String toJsonString();

  // Apply a config JSON to the in-memory settings (missing keys keep their
  // current values; out-of-range values are clamped). Returns false when the
  // body doesn't parse or contains none of the known keys. Does NOT save -
  // callers persist with saveConfigFile() and reboot to apply cleanly.
  bool applyFromJsonString(const String &body);
};

extern ProjectConfig projectConfig;

#endif // PROJECT_CONFIG_H
