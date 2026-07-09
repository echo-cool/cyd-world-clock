#ifndef GENERIC_BASE_PROJECT_H
#define GENERIC_BASE_PROJECT_H

#include <Arduino.h>
#include <ezTime.h>

#include "boardProfile.h"
#include "projectConfig.h"
#include "projectDisplay.h"

// Backlight control pin for the selected board profile.
#define BACKLIGHT_PIN BOARD_BACKLIGHT_PIN

// NTP sync monitoring variables (updated by handleTimeSync in genericBaseProject.cpp)
extern unsigned long lastSyncTime;
extern unsigned long syncCount;
extern bool ntpSyncStatus;

// Hostname of the NTP server currently in use. The pool behind it includes
// mainland-China-reachable servers; while the clock has never synced, the
// selection rotates through the pool until a server answers.
const char *currentNtpServer();

extern ProjectDisplay *projectDisplay;

extern Timezone myTZ;

// Double-reset detector: created in baseProjectSetup(), polled by
// baseProjectLoop(), and stopped before any reboot so the restart isn't read as
// a double reset. Two resets within DRD_TIMEOUT force the setup portal open.
// (Defined in genericBaseProject.cpp; used by setupPortal.cpp too.)
class DoubleResetDetector;
extern DoubleResetDetector *drd;

void baseProjectSetup();
void baseProjectLoop();

#endif // GENERIC_BASE_PROJECT_H
