#include "projectDisplay.h"

#include <TFT_eSPI.h>
// A library for interfacing with LCD displays
//
// Can be installed from the library manager (Search for "TFT_eSPI")
// https://github.com/Bodmer/TFT_eSPI

TFT_eSPI tft = TFT_eSPI();

class CheapYellowDisplay : public ProjectDisplay
{
public:
  void displaySetup()
  {

    Serial.println("cyd display setup");
    setWidth(320);
    setHeight(240);

    // Start the tft display and set it to black
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    tft.setTextFont(2);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("System initializing...", 160, 120);
  }

  void drawWifiManagerMessage(WiFiManager *myWiFiManager)
  {
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    Serial.println("Entered Conf Mode");
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
};
