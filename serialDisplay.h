#ifndef SERIAL_DISPLAY_H
#define SERIAL_DISPLAY_H

#include <Arduino.h>

#include "logBuffer.h"
#include "projectDisplay.h"

class SerialDisplay : public ProjectDisplay
{
public:
  void displaySetup()
  {
    Log.println("Serial Display Setup");
  }

  void drawWifiManagerMessage(WiFiManager *myWiFiManager)
  {
    Log.println("Entered Conf Mode");
  }
};

#endif // SERIAL_DISPLAY_H
