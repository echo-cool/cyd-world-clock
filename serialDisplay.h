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
};

#endif // SERIAL_DISPLAY_H
