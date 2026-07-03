#ifndef WIFI_MANAGER_HANDLER_H
#define WIFI_MANAGER_HANDLER_H

class DoubleResetDetector;
class ProjectConfig;
class ProjectDisplay;

// Created in baseProjectSetup(); also stopped from setupWiFiManager().
extern DoubleResetDetector *drd;

// config is passed by reference so that values entered in the captive portal
// update the live, in-memory projectConfig (not just a throwaway copy).
void setupWiFiManager(bool forceConfig, ProjectConfig &config, ProjectDisplay *theDisplay);

#endif // WIFI_MANAGER_HANDLER_H
