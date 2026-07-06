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
#define PROJECT_SHOW_GRID "showGrid"
#define PROJECT_HOSTNAME "hostname"
#define PROJECT_NIGHT_START "nightStartHour"
#define PROJECT_NIGHT_END "nightEndHour"
#define PROJECT_NIGHT_BRIGHTNESS "nightBrightness"
#define PROJECT_AUTO_BRIGHTNESS "autoBrightness"
#define PROJECT_SMOOTH_FONT "smoothFont"
#define PROJECT_DAYNIGHT_ICONS "dayNightIcons"
#define PROJECT_HOME_MARKER "homeMarker"
#define PROJECT_QUAD_WEATHER "quadWeather"
#define PROJECT_DAYLIGHT_BAR "daylightBar"
#define PROJECT_MARKET_BAR "marketBar"
#define PROJECT_WEATHER_ALERTS "weatherAlerts"
#define PROJECT_MAC_OVERRIDE "macOverride"
#define PROJECT_USE_FAHRENHEIT "useFahrenheit"
#define PROJECT_FLIP_DISPLAY "flipDisplay"
#define PROJECT_WEEK_START_MONDAY "weekStartMonday"
#define PROJECT_WEATHER_REFRESH_MIN "weatherRefreshMin"

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

  // Divider grid between the four quadrants of the world-clock face.
  bool showGrid = false;

  // mDNS / OTA hostname ("<hostname>.local"). Changeable from the web
  // settings page so two clocks on one network don't collide; applied on the
  // next boot (mDNS registers during setup).
  String hostname = "esp32worldclock";

  // Optional custom STA MAC address ("AA:BB:CC:DD:EE:FF", empty = factory MAC).
  // Lets the clock impersonate a device that a login-required (captive-portal)
  // network has already authorized, since that access is granted per MAC. The
  // ESP32 forgets a custom MAC across reboots, so it is re-applied every boot
  // (netCheck.cpp applyStaMacOverride) and changing it requires a reboot.
  String staMacOverride = "";

  // Night dimming, used by auto-brightness (ClockLogic.cpp):
  //  - autoBrightness is the master switch: off = the backlight always stays
  //    at the user's set brightness (light sensor and schedule both ignored).
  //  - nightBrightness (1-255) is the backlight target in a dark room (light
  //    sensor) or inside the schedule window below (sensor fallback).
  //  - The window is in home-zone hours; start == end disables the schedule
  //    (the light sensor, when trusted, works regardless).
  bool autoBrightness = true;
  int nightStartHour = 1;
  int nightEndHour = 7;
  int nightBrightness = 1;

  // Home-screen extras, each individually revertible from the web settings
  // page (all default on; turning one off restores the previous look):
  //  - dayNightIcons: sun/moon glyph per quadrant, and readable ice-blue
  //    evening/night text colors on every face (off = the legacy greys,
  //    no icons)
  //  - homeMarker: accent border around the home (top-left) quadrant
  //  - quadWeather: temperature + condition color on the quad face's date
  //    lines (uses the weather data the background task already fetches)
  //  - daylightBar: per-quadrant day gradient bar (sunrise/sunset from the
  //    city's real solar position) with a tick at the current time
  //  - marketProgressBar: trading-day progress bar along a quadrant's
  //    bottom edge while its exchange is inside regular hours
  //  - smoothTimeFont: anti-aliased digits (fontTimeDigits.h) for the quad
  //    face's times instead of the pixel-doubled Font 4
  //  - weatherAlerts: show a weather alert on a quadrant's market status line
  //    (US cities: official NWS warnings; others: severe conditions from the
  //    weather code). Alternates with the market status when both are present.
  bool smoothTimeFont = true;
  bool dayNightIcons = true;
  bool homeMarker = true;
  bool quadWeather = true;
  bool daylightBar = true;
  bool marketProgressBar = true;
  bool weatherAlerts = true;

  // Temperatures in Fahrenheit instead of Celsius (weather face, quadrant
  // weather). Stored values stay Celsius; only the display converts.
  bool useFahrenheit = false;

  // Rotate the whole UI 180 degrees (touch included) for displays mounted
  // upside down. Applied from the first boot screen onward.
  bool flipDisplay = false;

  // Calendar face: start the week on Monday instead of Sunday.
  bool weekStartMonday = false;

  // Minutes between weather fetches (5-120). Failures retry sooner
  // regardless (weatherService.cpp).
  int weatherRefreshMin = 20;

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
