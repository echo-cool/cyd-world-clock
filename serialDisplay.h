#ifndef SERIAL_DISPLAY_H
#define SERIAL_DISPLAY_H

#include <Arduino.h>

#include "projectDisplay.h"

class SerialDisplay : public ProjectDisplay
{
public:
  void displaySetup()
  {
    Serial.println("Serial Display Setup");
  }

  void drawWifiManagerMessage(WiFiManager *myWiFiManager)
  {
    Serial.println("Entered Conf Mode");
  }
};

#endif // SERIAL_DISPLAY_H
