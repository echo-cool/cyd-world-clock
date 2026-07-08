#include "projectConfig.h"

#include <FS.h>
#include "SPIFFS.h"
#include <ArduinoJson.h>

#include "clockFaces.h"    // FACE_COUNT - clamp the saved face index
#include "logBuffer.h"     // Log
#include "textSanitize.h"  // puretext::sanitizeHostname - host-tested core

ProjectConfig projectConfig;

// Thin String wrapper over the host-tested puretext::sanitizeHostname.
String sanitizeHostname(const String &raw)
{
  return String(puretext::sanitizeHostname(raw.c_str()).c_str());
}

// Serialize all settings into json. Shared by saveConfigFile and the
// /api/config backup so the two can never drift apart.
static void fillJson(ProjectConfig &c, JsonDocument &json)
{
  json[PROJECT_TIME_ZONE_LABEL] = c.timeZone;
  json[PROJECT_TIME_TWENTY_FOUR_HOUR] = c.twentyFourHour;
  json[PROJECT_TIME_US_DATE] = c.usDateFormat;

  for (int i = 0; i < 4; i++)
  {
    json[String(PROJECT_ZONE_NAME_PREFIX) + String(i)] = c.zoneName[i];
    json[String(PROJECT_ZONE_TZ_PREFIX) + String(i)] = c.zoneTZ[i];
  }

  json[PROJECT_BRIGHTNESS] = c.brightness;
  json[PROJECT_CLOCK_FACE] = c.clockFace;
  json[PROJECT_SHOW_GRID] = c.showGrid;
  json[PROJECT_HOSTNAME] = c.hostname;
  json[PROJECT_MAC_OVERRIDE] = c.staMacOverride;
  json[PROJECT_AUTO_BRIGHTNESS] = c.autoBrightness;
  json[PROJECT_NIGHT_START] = c.nightStartHour;
  json[PROJECT_NIGHT_END] = c.nightEndHour;
  json[PROJECT_NIGHT_BRIGHTNESS] = c.nightBrightness;
  json[PROJECT_SMOOTH_FONT] = c.smoothTimeFont;
  json[PROJECT_DAYNIGHT_ICONS] = c.dayNightIcons;
  json[PROJECT_HOME_MARKER] = c.homeMarker;
  json[PROJECT_QUAD_WEATHER] = c.quadWeather;
  json[PROJECT_DAYLIGHT_BAR] = c.daylightBar;
  json[PROJECT_MARKET_BAR] = c.marketProgressBar;
  json[PROJECT_WEATHER_ALERTS] = c.weatherAlerts;
  json[PROJECT_USE_FAHRENHEIT] = c.useFahrenheit;
  json[PROJECT_FLIP_DISPLAY] = c.flipDisplay;
  json[PROJECT_WEEK_START_MONDAY] = c.weekStartMonday;
  json[PROJECT_WEATHER_REFRESH_MIN] = c.weatherRefreshMin;
}

// Apply json onto the settings; missing keys keep their current values and
// out-of-range values are clamped. Shared by fetchConfigFile and the
// /api/config restore. Returns true if at least one known key was present.
static bool applyDoc(ProjectConfig &c, JsonDocument &json)
{
  bool any = false;

  if (json.containsKey(PROJECT_TIME_ZONE_LABEL))
  {
    c.timeZone = String(json[PROJECT_TIME_ZONE_LABEL].as<String>());
    any = true;
  }

  if (json.containsKey(PROJECT_TIME_TWENTY_FOUR_HOUR))
  {
    c.twentyFourHour = json[PROJECT_TIME_TWENTY_FOUR_HOUR].as<bool>();
    any = true;
  }

  if (json.containsKey(PROJECT_TIME_US_DATE))
  {
    c.usDateFormat = json[PROJECT_TIME_US_DATE].as<bool>();
    any = true;
  }

  for (int i = 0; i < 4; i++)
  {
    String nameKey = String(PROJECT_ZONE_NAME_PREFIX) + String(i);
    String tzKey = String(PROJECT_ZONE_TZ_PREFIX) + String(i);
    if (json.containsKey(nameKey))
    {
      c.zoneName[i] = json[nameKey].as<String>();
      any = true;
    }
    if (json.containsKey(tzKey))
    {
      c.zoneTZ[i] = json[tzKey].as<String>();
      any = true;
    }
  }

  if (json.containsKey(PROJECT_BRIGHTNESS))
  {
    c.brightness = constrain(json[PROJECT_BRIGHTNESS].as<int>(), 1, 255);
    any = true;
  }

  if (json.containsKey(PROJECT_CLOCK_FACE))
  {
    c.clockFace = constrain(json[PROJECT_CLOCK_FACE].as<int>(), 0, FACE_COUNT - 1);
    any = true;
  }

  if (json.containsKey(PROJECT_SHOW_GRID))
  {
    c.showGrid = json[PROJECT_SHOW_GRID].as<bool>();
    any = true;
  }

  if (json.containsKey(PROJECT_HOSTNAME))
  {
    c.hostname = sanitizeHostname(json[PROJECT_HOSTNAME].as<String>());
    any = true;
  }

  if (json.containsKey(PROJECT_MAC_OVERRIDE))
  {
    c.staMacOverride = String(json[PROJECT_MAC_OVERRIDE].as<String>());
    any = true;
  }

  if (json.containsKey(PROJECT_AUTO_BRIGHTNESS))
  {
    c.autoBrightness = json[PROJECT_AUTO_BRIGHTNESS].as<bool>();
    any = true;
  }

  if (json.containsKey(PROJECT_NIGHT_START))
  {
    c.nightStartHour = constrain(json[PROJECT_NIGHT_START].as<int>(), 0, 23);
    any = true;
  }

  if (json.containsKey(PROJECT_NIGHT_END))
  {
    c.nightEndHour = constrain(json[PROJECT_NIGHT_END].as<int>(), 0, 23);
    any = true;
  }

  if (json.containsKey(PROJECT_NIGHT_BRIGHTNESS))
  {
    c.nightBrightness = constrain(json[PROJECT_NIGHT_BRIGHTNESS].as<int>(), 1, 255);
    any = true;
  }

  // Bool toggles (home-screen extras + display/weather preferences)
  struct { const char *key; bool *value; } toggles[] = {
      {PROJECT_SMOOTH_FONT, &c.smoothTimeFont},
      {PROJECT_DAYNIGHT_ICONS, &c.dayNightIcons},
      {PROJECT_HOME_MARKER, &c.homeMarker},
      {PROJECT_QUAD_WEATHER, &c.quadWeather},
      {PROJECT_DAYLIGHT_BAR, &c.daylightBar},
      {PROJECT_MARKET_BAR, &c.marketProgressBar},
      {PROJECT_WEATHER_ALERTS, &c.weatherAlerts},
      {PROJECT_USE_FAHRENHEIT, &c.useFahrenheit},
      {PROJECT_FLIP_DISPLAY, &c.flipDisplay},
      {PROJECT_WEEK_START_MONDAY, &c.weekStartMonday},
  };
  for (auto &toggle : toggles)
  {
    if (json.containsKey(toggle.key))
    {
      *toggle.value = json[toggle.key].as<bool>();
      any = true;
    }
  }

  if (json.containsKey(PROJECT_WEATHER_REFRESH_MIN))
  {
    c.weatherRefreshMin = constrain(json[PROJECT_WEATHER_REFRESH_MIN].as<int>(), 5, 120);
    any = true;
  }

  return any;
}

bool ProjectConfig::fetchConfigFile()
{
  if (SPIFFS.exists(PROJECT_CONFIG_JSON))
  {
    // file exists, reading and loading
    Log.println("reading config file");
    File configFile = SPIFFS.open(PROJECT_CONFIG_JSON, "r");
    if (configFile)
    {
      Log.println("opened config file");
      StaticJsonDocument<2048> json;
      DeserializationError error = deserializeJson(json, configFile);
      serializeJsonPretty(json, Serial);
      if (!error)
      {
        Log.println("\nparsed json");
        applyDoc(*this, json);
        return true;
      }
      else
      {
        Log.println("failed to load json config");
        return false;
      }
    }
  }

  Log.println("Config file does not exist");
  return false;
}

bool ProjectConfig::saveConfigFile()
{
  Log.println(F("Saving config"));
  StaticJsonDocument<2048> json;
  fillJson(*this, json);

  File configFile = SPIFFS.open(PROJECT_CONFIG_JSON, "w");
  if (!configFile)
  {
    Log.println("failed to open config file for writing");
    return false;
  }

  serializeJsonPretty(json, Serial);
  if (serializeJson(json, configFile) == 0)
  {
    Log.println(F("Failed to write to file"));
    return false;
  }
  configFile.close();
  return true;
}

String ProjectConfig::toJsonString()
{
  StaticJsonDocument<2048> json;
  fillJson(*this, json);
  String out;
  serializeJsonPretty(json, out);
  return out;
}

bool ProjectConfig::applyFromJsonString(const String &body)
{
  StaticJsonDocument<2048> json;
  if (deserializeJson(json, body))
  {
    return false;
  }
  return applyDoc(*this, json);
}
