#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

#include <Arduino.h>

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
  // Defaults match the compiled-in worldZones in ClockLogic.cpp.
  String zoneName[4] = {"SANTA CLARA", "NEW YORK", "BEIJING", "LONDON"};
  String zoneTZ[4] = {"America/Los_Angeles", "America/New_York",
                      "Asia/Shanghai", "Europe/London"};

  bool fetchConfigFile();
  bool saveConfigFile();
};

extern ProjectConfig projectConfig;

#endif // PROJECT_CONFIG_H
