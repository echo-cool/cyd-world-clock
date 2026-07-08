#ifndef CHEAP_YELLOW_LCD_H
#define CHEAP_YELLOW_LCD_H

#include <TFT_eSPI.h>
// A library for interfacing with LCD displays
//
// Can be installed from the library manager (Search for "TFT_eSPI")
// https://github.com/Bodmer/TFT_eSPI

#include "projectDisplay.h"

// The one TFT driver instance for the whole project (defined in cheapYellowLCD.cpp).
extern TFT_eSPI tft;

class CheapYellowDisplay : public ProjectDisplay
{
public:
  void displaySetup();
};

#endif // CHEAP_YELLOW_LCD_H
