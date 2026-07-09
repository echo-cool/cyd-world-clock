// TFT_eSPI includes FS.h with FS_NO_GLOBALS when SMOOTH_FONT is enabled, which
// hides the global FS/File aliases some code expects. Include FS.h first (before
// cheapYellowLCD.h pulls in TFT_eSPI) so those globals stay available.
#include <FS.h>

#include "cheapYellowLCD.h"
#include "boardProfile.h"
#include "deviceIdentity.h"
#include "logBuffer.h"
#include "projectConfig.h" // flipDisplay - panel orientation

TFT_eSPI tft = TFT_eSPI();

void CheapYellowDisplay::displaySetup()
{

  Log.println(String("display setup: ") + BOARD_PROFILE_NAME);
  setWidth(BOARD_DISPLAY_WIDTH);
  setHeight(BOARD_DISPLAY_HEIGHT);

  // Start the tft display and set it to black. Board profiles supply the
  // rotations that make the UI coordinate system landscape.
  tft.init();
  tft.setRotation(projectConfig.flipDisplay ? BOARD_TFT_ROTATION_FLIPPED
                                            : BOARD_TFT_ROTATION_NORMAL);
  tft.fillScreen(TFT_BLACK);

  // Compact title at the top: the area below is the boot console (uiPages),
  // which mirrors the log so startup progress is visible on the device.
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("System initializing...", BOARD_DISPLAY_WIDTH / 2, 2);

  // Device identity is shown immediately so two clocks in setup mode are easy
  // to tell apart before the phone joins the setup hotspot.
  tft.setTextFont(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(deviceLabel(), BOARD_DISPLAY_WIDTH / 2, 18);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(deviceMacAddress(), BOARD_DISPLAY_WIDTH / 2, 28);

  tft.drawFastHLine(0, 38, BOARD_DISPLAY_WIDTH, TFT_DARKGREY);
}
