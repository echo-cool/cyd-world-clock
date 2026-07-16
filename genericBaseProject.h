#ifndef GENERIC_BASE_PROJECT_H
#define GENERIC_BASE_PROJECT_H

#include <Arduino.h>
#include <ezTime.h>

#include "boardProfile.h"
#include "projectConfig.h"
#include "projectDisplay.h"

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

// The double-reset detector now lives in drdGuard.h - deliberate reboots must
// go through rebootCleanly() so they are not read as a double reset.

void baseProjectSetup();
void baseProjectLoop();

#endif // GENERIC_BASE_PROJECT_H
