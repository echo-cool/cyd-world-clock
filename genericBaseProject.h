#ifndef GENERIC_BASE_PROJECT_H
#define GENERIC_BASE_PROJECT_H

#include <Arduino.h>
#include <ezTime.h>

#include "projectConfig.h"
#include "projectDisplay.h"

// Backlight control pin (CYD / ESP32-Cheap-Yellow-Display)
#define BACKLIGHT_PIN 21

// NTP sync monitoring variables (updated by handleTimeSync in genericBaseProject.cpp)
extern unsigned long lastSyncTime;
extern unsigned long syncCount;
extern bool ntpSyncStatus;

extern ProjectDisplay *projectDisplay;

extern Timezone myTZ;

void baseProjectSetup();
void baseProjectLoop();

#endif // GENERIC_BASE_PROJECT_H
