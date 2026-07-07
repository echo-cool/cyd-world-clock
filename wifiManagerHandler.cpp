// Needs to be defined before the ESP_DoubleResetDetector import
#define ESP_DRD_USE_SPIFFS true

#include <WiFiManager.h>
#include <ESP_DoubleResetDetector.h>
#include <WiFi.h>

#include "wifiManagerHandler.h"
#include "logBuffer.h"
#include "projectConfig.h"
#include "projectDisplay.h"
#include "netCheck.h" // applyStaMacOverride, normalizeMac
#include "uiPages.h"  // bootReportWifiFailure - on-screen join-failure page

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

  // Optional custom MAC for login-required networks (see netCheck.h). Leave
  // blank for the factory MAC.
  WiFiManagerParameter macParam(PROJECT_MAC_OVERRIDE,
                                "Custom MAC (login networks, blank = default)",
                                config.staMacOverride.c_str(), 20);

  wm.addParameter(&timeZoneParam);
  wm.addParameter(&isTwentyFourHour);
  wm.addParameter(&isUsDateFormat);
  wm.addParameter(&macParam);

  // Make sure the (optional) cloned MAC is on the STA interface before
  // WiFiManager tries the stored credentials or the portal connects. This also
  // initialises WiFi for the forceConfig path (which skips the preconfigured
  // connect block that would otherwise have done WiFi.mode(WIFI_STA)).
  WiFi.mode(WIFI_STA);
  applyStaMacOverride();

  // Exit the portal as soon as the user saves credentials, even when the
  // join fails - the on-screen failure page below then explains why, instead
  // of the portal silently sitting there until its timeout reboots the clock.
  wm.setBreakAfterConfig(true);

  bool portalOk;
  if (forceConfig)
  {
    // IF we forced config this time, lets stop the double reset so it doesn't get stuck in a loop
    drd->stop();
    portalOk = wm.startConfigPortal("esp32Project", "12345678");
  }
  else
  {
    portalOk = wm.autoConnect("esp32Project", "12345678");
  }

  // Persist the portal's extra fields (timezone, formats, custom MAC)
  // whenever the user pressed Save - whether or not the WiFi join worked.
  if (shouldSaveConfig)
  {
    config.timeZone = String(timeZoneParam.getValue());
    config.twentyFourHour = (strncmp(isTwentyFourHour.getValue(), "T", 1) == 0);
    config.usDateFormat = (strncmp(isUsDateFormat.getValue(), "T", 1) == 0);
    config.staMacOverride = normalizeMac(String(macParam.getValue()));

    config.saveConfigFile();
  }

  if (!portalOk)
  {
    if (shouldSaveConfig && WiFi.status() != WL_CONNECTED)
    {
      // The user just typed credentials into the portal and the join failed.
      // Don't reboot-loop: hand the details to the UI, which shows a page
      // with the reason plus Reboot / Settings (status & logs) buttons; the
      // rest of boot continues with its network waits skipped.
      int st = wm.getLastConxResult();
      Log.println("Portal WiFi \"" + wm.getWiFiSSID() + "\" failed to join "
                  "(wifi status " + String(st) + ") - showing the failure page");
      bootReportWifiFailure(wm.getWiFiSSID(), st);
      return;
    }

    Log.println("Config portal timed out with no WiFi - rebooting to retry");
    delay(3000);
    // Stop the double-reset detector first: its RTC flag is still armed
    // (drd->loop() never ran during setup), so restarting without this
    // would read as a "double reset" and force the portal open again,
    // looping forever instead of retrying the preconfigured WiFi.
    drd->stop();
    // reset and try again (preconfigured credentials get retried first)
    ESP.restart();
    delay(5000);
  }

  if (shouldSaveConfig)
  {
    // Connected with freshly saved settings: reboot so hostname, custom MAC
    // and timezone all apply through the normal boot path.
    drd->stop();
    ESP.restart();
    delay(5000);
  }
}
