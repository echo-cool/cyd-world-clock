// TFT_eSPI includes FS.h with FS_NO_GLOBALS when SMOOTH_FONT is enabled, which
// hides the global FS/File aliases some code expects. Include FS.h first (before
// cheapYellowLCD.h pulls in TFT_eSPI) so those globals stay available.
#include <FS.h>

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
