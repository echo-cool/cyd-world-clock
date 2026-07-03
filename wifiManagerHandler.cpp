// Needs to be defined before the ESP_DoubleResetDetector import
#define ESP_DRD_USE_SPIFFS true

#include <WiFiManager.h>
#include <ESP_DoubleResetDetector.h>

#include "wifiManagerHandler.h"
#include "logBuffer.h"
#include "projectConfig.h"
#include "projectDisplay.h"

DoubleResetDetector *drd;
ProjectDisplay *wm_Display;

// flag for saving data
bool shouldSaveConfig = false;

// callback notifying us of the need to save config
void saveConfigCallback()
{
  Log.println("Should save config");
  shouldSaveConfig = true;
}

void configModeCallback(WiFiManager *myWiFiManager)
{
  wm_Display->drawWifiManagerMessage(myWiFiManager);
}

// If nobody uses the captive portal within this window, reboot and retry the
// whole connection sequence (preconfigured credentials first) instead of
// sitting in the portal forever. This lets the clock recover unattended when
// the router comes back after a power cut: without a timeout the portal
// blocks until someone walks over to the device.
#define CONFIG_PORTAL_TIMEOUT_S 300

void setupWiFiManager(bool forceConfig, ProjectConfig &config, ProjectDisplay *theDisplay)
{
  wm_Display = theDisplay;
  WiFiManager wm;
  // set config save notify callback
  wm.setSaveConfigCallback(saveConfigCallback);
  // set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wm.setAPCallback(configModeCallback);
  // reboot-and-retry instead of blocking in the portal forever
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_S);

  WiFiManagerParameter timeZoneParam(PROJECT_TIME_ZONE_LABEL, "Time Zone", config.timeZone.c_str(), 60);

  char checkBox[] = "type=\"checkbox\"";
  char checkBoxChecked[] = "type=\"checkbox\" checked";
  char *customHtml;

  if (config.twentyFourHour)
  {
    customHtml = checkBoxChecked;
  }
  else
  {
    customHtml = checkBox;
  }
  WiFiManagerParameter isTwentyFourHour(PROJECT_TIME_TWENTY_FOUR_HOUR, "24H Clock", "T", 2, customHtml);

  char *customHtmlTwo;
  if (config.usDateFormat)
  {
    customHtmlTwo = checkBoxChecked;
  }
  else
  {
    customHtmlTwo = checkBox;
  }
  WiFiManagerParameter isUsDateFormat(PROJECT_TIME_US_DATE, "US Date Format", "T", 2, customHtmlTwo);

  wm.addParameter(&timeZoneParam);
  wm.addParameter(&isTwentyFourHour);
  wm.addParameter(&isUsDateFormat);

  if (forceConfig)
  {
    // IF we forced config this time, lets stop the double reset so it doesn't get stuck in a loop
    drd->stop();
    if (!wm.startConfigPortal("esp32Project", "12345678"))
    {
      Log.println("Config portal timed out with no WiFi - rebooting to retry");
      delay(3000);
      // reset and try again (preconfigured credentials get retried first)
      ESP.restart();
      delay(5000);
    }
  }
  else
  {
    if (!wm.autoConnect("esp32Project", "12345678"))
    {
      Log.println("Config portal timed out with no WiFi - rebooting to retry");
      delay(3000);
      // Stop the double-reset detector first: its RTC flag is still armed
      // (drd->loop() never ran during setup), so restarting without this
      // would read as a "double reset" and force the portal open again,
      // looping forever instead of retrying the preconfigured WiFi.
      drd->stop();
      ESP.restart();
      delay(5000);
    }
  }

  // save the custom parameters to FS
  if (shouldSaveConfig)
  {

    config.timeZone = String(timeZoneParam.getValue());
    config.twentyFourHour = (strncmp(isTwentyFourHour.getValue(), "T", 1) == 0);
    config.usDateFormat = (strncmp(isUsDateFormat.getValue(), "T", 1) == 0);

    config.saveConfigFile();
    drd->stop();
    ESP.restart();
    delay(5000);
  }
}
