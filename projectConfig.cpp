#include "projectConfig.h"

#include <FS.h>
#include "SPIFFS.h"
#include <ArduinoJson.h>

#include "clockFaces.h" // FACE_COUNT - clamp the saved face index

ProjectConfig projectConfig;

bool ProjectConfig::fetchConfigFile()
{
  if (SPIFFS.exists(PROJECT_CONFIG_JSON))
  {
    // file exists, reading and loading
    Serial.println("reading config file");
    File configFile = SPIFFS.open(PROJECT_CONFIG_JSON, "r");
    if (configFile)
    {
      Serial.println("opened config file");
      StaticJsonDocument<2048> json;
      DeserializationError error = deserializeJson(json, configFile);
      serializeJsonPretty(json, Serial);
      if (!error)
      {
        Serial.println("\nparsed json");

        if (json.containsKey(PROJECT_TIME_ZONE_LABEL))
        {
          timeZone = String(json[PROJECT_TIME_ZONE_LABEL].as<String>());
        }

        if (json.containsKey(PROJECT_TIME_TWENTY_FOUR_HOUR))
        {
          twentyFourHour = json[PROJECT_TIME_TWENTY_FOUR_HOUR].as<bool>();
        }

        if (json.containsKey(PROJECT_TIME_US_DATE))
        {
          usDateFormat = json[PROJECT_TIME_US_DATE].as<bool>();
        }

        for (int i = 0; i < 4; i++)
        {
          String nameKey = String(PROJECT_ZONE_NAME_PREFIX) + String(i);
          String tzKey = String(PROJECT_ZONE_TZ_PREFIX) + String(i);
          if (json.containsKey(nameKey))
          {
            zoneName[i] = json[nameKey].as<String>();
          }
          if (json.containsKey(tzKey))
          {
            zoneTZ[i] = json[tzKey].as<String>();
          }
        }

        if (json.containsKey(PROJECT_BRIGHTNESS))
        {
          brightness = constrain(json[PROJECT_BRIGHTNESS].as<int>(), 1, 255);
        }

        if (json.containsKey(PROJECT_CLOCK_FACE))
        {
          clockFace = constrain(json[PROJECT_CLOCK_FACE].as<int>(), 0, FACE_COUNT - 1);
        }

        return true;
      }
      else
      {
        Serial.println("failed to load json config");
        return false;
      }
    }
  }

  Serial.println("Config file does not exist");
  return false;
}

bool ProjectConfig::saveConfigFile()
{
  Serial.println(F("Saving config"));
  StaticJsonDocument<2048> json;
  json[PROJECT_TIME_ZONE_LABEL] = timeZone;
  json[PROJECT_TIME_TWENTY_FOUR_HOUR] = twentyFourHour;
  json[PROJECT_TIME_US_DATE] = usDateFormat;

  for (int i = 0; i < 4; i++)
  {
    json[String(PROJECT_ZONE_NAME_PREFIX) + String(i)] = zoneName[i];
    json[String(PROJECT_ZONE_TZ_PREFIX) + String(i)] = zoneTZ[i];
  }

  json[PROJECT_BRIGHTNESS] = brightness;
  json[PROJECT_CLOCK_FACE] = clockFace;

  File configFile = SPIFFS.open(PROJECT_CONFIG_JSON, "w");
  if (!configFile)
  {
    Serial.println("failed to open config file for writing");
    return false;
  }

  serializeJsonPretty(json, Serial);
  if (serializeJson(json, configFile) == 0)
  {
    Serial.println(F("Failed to write to file"));
    return false;
  }
  configFile.close();
  return true;
}
