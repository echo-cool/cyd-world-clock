// WiFiManager (via WebServer.h) needs the global FS/File aliases from FS.h,
// but TFT_eSPI includes FS.h with FS_NO_GLOBALS when SMOOTH_FONT is enabled.
// WiFiManager must therefore be included before cheapYellowLCD.h (TFT_eSPI).
#include <WiFi.h>
#include <WiFiManager.h>

#include "cheapYellowLCD.h"
#include "logBuffer.h"
#include "projectConfig.h" // flipDisplay - panel orientation

TFT_eSPI tft = TFT_eSPI();

void CheapYellowDisplay::displaySetup()
{

  Log.println("cyd display setup");
  setWidth(320);
  setHeight(240);

  // Start the tft display and set it to black. Rotation 1 is the CYD's
  // native landscape; 3 is the same panel mounted upside down (flipDisplay).
  tft.init();
  tft.setRotation(projectConfig.flipDisplay ? 3 : 1);
  tft.fillScreen(TFT_BLACK);

  // Compact title at the top: the area below is the boot console (uiPages),
  // which mirrors the log so startup progress is visible on the device.
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("System initializing...", 160, 2);
  tft.drawFastHLine(0, 20, 320, TFT_DARKGREY);
}

void CheapYellowDisplay::drawWifiManagerMessage(WiFiManager *myWiFiManager)
{
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);
  Log.println("Entered Conf Mode");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Entered Conf Mode:", screenCenterX, 5, 2);
  tft.drawString("Connect to the following WIFI AP:", 5, 28, 2);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.drawString(myWiFiManager->getConfigPortalSSID(), 20, 48, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Password:", 5, 64, 2);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.drawString("12345678", 20, 82, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.drawString("If it doesn't AutoConnect, use this IP:", 5, 110, 2);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.drawString(WiFi.softAPIP().toString(), 20, 128, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}
