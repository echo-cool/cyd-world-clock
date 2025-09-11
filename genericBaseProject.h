// ----------------------------
// Library Defines - Need to be defined before library import
// ----------------------------

#define ESP_DRD_USE_SPIFFS true

// ----------------------------
// Configuration
// ----------------------------

// Preconfigured WiFi credentials - try these first, fallback to WiFiManager if failed
#define PRECONFIGURED_SSID "watermelonCrystal"
#define PRECONFIGURED_PASSWORD "2479CrystalDrive"
#define PRECONFIGURED_TIMEZONE "America/New_York"
#define WIFI_CONNECT_TIMEOUT 5000  // 10 seconds timeout for WiFi connection

// ----------------------------
// Standard Libraries
// ----------------------------
#include <WiFi.h>

#include <FS.h>
#include "SPIFFS.h"

// Multi-core task handles
TaskHandle_t TimeSyncTask;

// NTP sync monitoring variables
unsigned long lastSyncTime = 0;
unsigned long syncCount = 0;
bool ntpSyncStatus = false;

// Function to manually test NTP sync - defined after ezTime includes

// ----------------------------
// Additional Libraries - each one of these will need to be installed.
// ----------------------------

#include <WiFiManager.h>
// Captive portal for configuring the WiFi

// If installing from the library manager (Search for "WifiManager")
// https://github.com/tzapu/WiFiManager

#include <ESP_DoubleResetDetector.h>
// A library for checking if the reset button has been pressed twice
// Can be used to enable config mode
// Can be installed from the library manager (Search for "ESP_DoubleResetDetector")
// https://github.com/khoih-prog/ESP_DoubleResetDetector

#include <ezTime.h>
// Library used for getting the time and converting session time
// to users timezone

// Search for "ezTime" in the Arduino Library manager
// https://github.com/ropg/ezTime

#include <ArduinoJson.h>
// Library used for parsing Json from the API responses

// Search for "Arduino Json" in the Arduino Library manager
// https://github.com/bblanchon/ArduinoJson

// ----------------------------
// Internal includes
// ----------------------------

#include "projectConfig.h"

#include "projectDisplay.h"

#include "wifiManagerHandler.h"

#include "cheapYellowLCD.h"

// Number of seconds after reset during which a
// subseqent reset will be considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

ProjectConfig projectConfig;

CheapYellowDisplay cyd;
ProjectDisplay *projectDisplay = &cyd;

Timezone myTZ;

// Production NTP sync monitoring - manual test function removed

// Time sync task running on Core 0
void timeSyncTaskCode(void * parameter) {
    Serial.println("Time sync task started on core " + String(xPortGetCoreID()));
    
    unsigned long lastStatusReport = 0;
    time_t lastKnownTime = 0;
    
    for(;;) {
        // Store time before events() call
        time_t timeBefore = UTC.now();
        
        // Handle ezTime events for NTP synchronization
        events();
        
        // Check if time was updated (indicates sync occurred)
        time_t timeAfter = UTC.now();
        
        // Detect if a sync just happened
        if (timeAfter != lastKnownTime && timeAfter > 1000000000) { // Valid timestamp
            if (lastKnownTime > 0 && abs(timeAfter - timeBefore) > 1) {
                // Time jumped significantly - likely a sync occurred
                syncCount++;
                lastSyncTime = millis();
                ntpSyncStatus = true;
                
                Serial.println("NTP Sync #" + String(syncCount) + " - " + UTC.dateTime() + " (Uptime: " + String(millis()/1000/60) + "min)");
            }
            lastKnownTime = timeAfter;
        }
        
        // Report sync status every 30 minutes (production frequency)
        if (millis() - lastStatusReport > 1800000) { // 30 minutes
            lastStatusReport = millis();
            
            Serial.println("NTP Status: " + String(syncCount) + " syncs, Last: " + String((millis() - lastSyncTime)/1000/60) + "min ago");
        }
        
        // Small delay to prevent excessive CPU usage
        vTaskDelay(100 / portTICK_PERIOD_MS); // 100ms delay
    }
}

void baseProjectSetup()
{
    projectDisplay->displaySetup();

    bool forceConfig = false;
    bool wifiConnected = false;

    // Initialize double reset detector
    drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);
    if (drd->detectDoubleReset())
    {
        Serial.println(F("Forcing config mode as there was a Double reset detected"));
        forceConfig = true;
    }

    // Initialize SPIFFS
    bool spiffsInitSuccess = SPIFFS.begin(false) || SPIFFS.begin(true);
    if (!spiffsInitSuccess)
    {
        Serial.println("SPIFFS initialisation failed!");
        while (1)
            yield(); // Stay here twiddling thumbs waiting
    }
    Serial.println("\r\nInitialisation done.");

    // Try to load existing config first
    if (!projectConfig.fetchConfigFile())
    {
        Serial.println("No saved config found, will use defaults or WiFiManager");
    }

    // Step 1: Try preconfigured WiFi credentials first
    if (!forceConfig)
    {
        Serial.println("Attempting to connect with preconfigured WiFi...");
        WiFi.mode(WIFI_STA);
        WiFi.begin(PRECONFIGURED_SSID, PRECONFIGURED_PASSWORD);
        
        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECT_TIMEOUT)
        {
            delay(500);
            Serial.print(".");
        }
        
        if (WiFi.status() == WL_CONNECTED)
        {
            wifiConnected = true;
            Serial.println();
            Serial.println("Connected with preconfigured WiFi!");
            Serial.print("IP address: ");
            Serial.println(WiFi.localIP());
            
            // Use preconfigured settings if no saved config
            if (projectConfig.timeZone.length() == 0)
            {
                projectConfig.timeZone = PRECONFIGURED_TIMEZONE;
                projectConfig.twentyFourHour = false;
                projectConfig.usDateFormat = true;
                Serial.println("Using preconfigured timezone and settings");
            }
        }
        else
        {
            Serial.println();
            Serial.println("Preconfigured WiFi connection failed, falling back to WiFiManager");
            forceConfig = true;
        }
    }

    // Step 2: If preconfigured WiFi failed or forced config, use WiFiManager
    if (!wifiConnected)
    {
        setupWiFiManager(forceConfig, projectConfig, projectDisplay);
    }

    // Final check to ensure WiFi is connected
    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        delay(500);
    }

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    Serial.println("Waiting for time sync");

    // Configure NTP sync frequency for production
    setInterval(1800); // Sync every 30 minutes (production setting)
    setDebug(ERROR); // Only show errors, not all debug info
    
    Serial.println("Performing initial NTP sync...");
    
    waitForSync();
    
    Serial.println("Initial NTP sync complete!");

    Serial.println();
    Serial.println("UTC:             " + UTC.dateTime());

    myTZ.setLocation(projectConfig.timeZone);
    Serial.print(projectConfig.timeZone);
    Serial.print(F(":     "));
    Serial.println(myTZ.dateTime());
    Serial.println("-------------------------");
    
    // Create time sync task on Core 0 (Core 1 is used for main loop/display)
    xTaskCreatePinnedToCore(
        timeSyncTaskCode,   // Task function
        "TimeSyncTask",     // Name of task
        10000,              // Stack size of task (10KB)
        NULL,               // Parameter of the task
        1,                  // Priority of the task (1 = low priority)
        &TimeSyncTask,      // Task handle to keep track of created task
        0);                 // Pin task to core 0
    
    Serial.println("Time sync task created on Core 0");
}

void baseProjectLoop()
{
    drd->loop();
    // Note: events() now runs on Core 0 in dedicated time sync task
}
