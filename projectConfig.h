#define PROJECT_CONFIG_JSON "/project_config.json"

#define PROJECT_TIME_ZONE_LABEL "timeZone"
#define PROJECT_TIME_TWENTY_FOUR_HOUR "twentyFourHour"
#define PROJECT_TIME_US_DATE "usDate"
#define PROJECT_ZONE_NAME_PREFIX "zoneName"
#define PROJECT_ZONE_TZ_PREFIX "zoneTz"

class ProjectConfig
{
public:
  // https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
  String timeZone = "America/Los_Angeles"; // seems to be something wrong with Europe/Dublin

  bool twentyFourHour = false;

  bool usDateFormat = false;

  // Per-quadrant world clock timezones, selectable from the touch UI.
  // Defaults match the compiled-in worldZones in ClockLogic.h.
  String zoneName[4] = {"SANTA CLARA", "NEW YORK", "BEIJING", "LONDON"};
  String zoneTZ[4] = {"America/Los_Angeles", "America/New_York",
                      "Asia/Shanghai", "Europe/London"};

  bool fetchConfigFile()
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

  bool saveConfigFile()
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
};
