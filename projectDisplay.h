
#ifndef PROJECTDISPLAY_H
#define PROJECTDISPLAY_H

// Only pointers to WiFiManager cross this interface, so a forward declaration
// keeps this header light; implementations include <WiFiManager.h> themselves.
class WiFiManager;

class ProjectDisplay
{
public:
  virtual void displaySetup() = 0;

  virtual void drawWifiManagerMessage(WiFiManager *myWiFiManager) = 0;

  void setWidth(int w)
  {
    screenWidth = w;
    screenCenterX = screenWidth / 2;
  }

  void setHeight(int h)
  {
    screenHeight = h;
  }

protected:
  int screenWidth;
  int screenHeight;
  int screenCenterX;
};
#endif
