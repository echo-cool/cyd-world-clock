#define PROJECT_CONFIG_JSON "/project_config.json"

#define PROJECT_TIME_ZONE_LABEL "timeZone"
#define PROJECT_TIME_TWENTY_FOUR_HOUR "twentyFourHour"
#define PROJECT_TIME_US_DATE "usDate"

// Number of clock zones shown on the display (one per screen quadrant)
#define PROJECT_NUM_ZONES 4

// Market ids used by the market-status engine (see market catalog in ClockLogic.h)
#define MARKET_NONE 0
#define MARKET_NYSE 1
#define MARKET_SSE 2
#define MARKET_LSE 3

class ProjectConfig
{
public:
  // https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
  String timeZone = "America/Los_Angeles"; // legacy single-zone field (kept for compatibility)

  bool twentyFourHour = false;

  bool usDateFormat = false;

  // Per-zone configuration shown on the four quadrants. Editable at runtime via
  // the web config page and persisted to flash.
  String zoneLabels[PROJECT_NUM_ZONES] = {"SANTA CLARA", "NEW YORK", "BEIJING", "LONDON"};
  String zoneTimezones[PROJECT_NUM_ZONES] = {"America/Los_Angeles", "America/New_York", "Asia/Shanghai", "Europe/London"};
  int zoneMarketId[PROJECT_NUM_ZONES] = {MARKET_NONE, MARKET_NYSE, MARKET_SSE, MARKET_LSE};

  // Which zone is "home" (drives auto-brightness and the +1/-1 relative-date hint)
  int homeZoneIndex = 0;

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

          // Per-zone settings
          if (json.containsKey("zones"))
          {
            JsonArray zones = json["zones"].as<JsonArray>();
            int i = 0;
            for (JsonVariant zv : zones)
            {
              if (i >= PROJECT_NUM_ZONES) break;
              JsonObject z = zv.as<JsonObject>();
              if (z.containsKey("label")) zoneLabels[i] = String(z["label"].as<String>());
              if (z.containsKey("tz")) zoneTimezones[i] = String(z["tz"].as<String>());
              if (z.containsKey("market")) zoneMarketId[i] = z["market"].as<int>();
              i++;
            }
          }

          if (json.containsKey("home"))
          {
            int h = json["home"].as<int>();
            if (h >= 0 && h < PROJECT_NUM_ZONES) homeZoneIndex = h;
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

    JsonArray zones = json.createNestedArray("zones");
    for (int i = 0; i < PROJECT_NUM_ZONES; i++)
    {
      JsonObject z = zones.createNestedObject();
      z["label"] = zoneLabels[i];
      z["tz"] = zoneTimezones[i];
      z["market"] = zoneMarketId[i];
    }
    json["home"] = homeZoneIndex;

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
