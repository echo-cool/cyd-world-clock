#include "serialCommands.h"

#include <WiFi.h>
#include <ezTime.h>

#include "ClockLogic.h"         // backlightLevel, worldZones, manualBrightnessUntil
#include "factoryReset.h"       // factoryReset - FACTORYRESET command
#include "genericBaseProject.h" // BACKLIGHT_PIN
#include "holidayService.h"     // public holiday status
#include "logShipper.h"         // remote log push status
#include "marketHolidays.h"     // holiday calendar status / refresh
#include "weatherService.h"     // weatherAgeMinutes, weatherInvalidate

void handleSerialCommands()
{
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        command.toUpperCase();

        if (command.startsWith("BRIGHTNESS ")) {
            // Extract brightness value from command (e.g., "BRIGHTNESS 128")
            int spaceIndex = command.indexOf(' ');
            if (spaceIndex > 0) {
                String valueStr = command.substring(spaceIndex + 1);
                int newBrightness = valueStr.toInt();

                if (newBrightness >= 5 && newBrightness <= 255) {
                    backlightLevel = newBrightness;
                    analogWrite(BACKLIGHT_PIN, backlightLevel);
                    // Respect this manual setting before auto-brightness resumes
                    manualBrightnessUntil = millis() + MANUAL_BRIGHTNESS_HOLD_MS;
                    // Persist so the level survives a reboot
                    projectConfig.brightness = backlightLevel;
                    projectConfig.saveConfigFile();
                    Log.print("Brightness set to: ");
                    Log.println(backlightLevel);
                } else {
                    Log.println("Error: Brightness must be between 5 and 255");
                }
            } else {
                Log.println("Error: Usage: BRIGHTNESS <value>");
            }
        }
        else if (command == "SYNC") {
            // Query the current NTP server directly. (waitForSync() can't be
            // used as the check here: it returns true whenever the clock is
            // merely set - which since the build-time seeding it always is -
            // without proving the server answered.)
            Log.println("Forcing NTP time synchronization (server: " +
                        String(currentNtpServer()) + ")...");
            time_t t;
            unsigned long measuredAt;
            if (queryNTP(currentNtpServer(), t, measuredAt)) {
                UTC.setTime(t, millis() - measuredAt);
                Log.println("Time sync successful!");
                Log.println("UTC: " + UTC.dateTime());
                Log.println("Santa Clara: " + worldZones[0].tz.dateTime());
            } else {
                Log.println("Time sync failed: " + errorString());
            }
        }
        else if (command == "WIFI" || command == "IP") {
            Log.println("=== WiFi Information ===");
            Log.print("SSID: ");
            Log.println(WiFi.SSID());
            Log.print("IP Address: ");
            Log.println(WiFi.localIP());
            Log.print("Gateway: ");
            Log.println(WiFi.gatewayIP());
            Log.print("Subnet: ");
            Log.println(WiFi.subnetMask());
            Log.print("DNS: ");
            Log.println(WiFi.dnsIP());
            Log.print("Signal Strength (RSSI): ");
            Log.print(WiFi.RSSI());
            Log.println(" dBm");
            Log.print("MAC Address: ");
            Log.println(WiFi.macAddress());
            Log.print("Connection Status: ");
            if (WiFi.status() == WL_CONNECTED) {
                Log.println("Connected");
            } else {
                Log.println("Disconnected");
            }
        }
        else if (command == "LDR") {
            printLdrStatus();
        }
        else if (command == "WEATHER") {
            long age = weatherAgeMinutes();
            Log.println(age < 0 ? "No weather data yet."
                                   : "Weather data is " + String(age) + " min old.");
            weatherInvalidate();
            Log.println("Cache invalidated - the background task refetches within seconds.");
        }
        else if (command == "HOLIDAYS") {
            printMarketHolidaysStatus();
            marketHolidaysForceRefresh();
            printPublicHolidaysStatus();
        }
        else if (command == "LOGSHIP") {
            logShipperPrintStatus(Log);
        }
        else if (command == "FACTORYRESET") {
            Log.println("Factory reset: erasing all settings and WiFi credentials...");
            factoryReset(); // wipes NVS + SPIFFS and reboots - does not return
        }
        else if (command == "HELP" || command == "?") {
            Log.println("=== Available Commands ===");
            Log.println("BRIGHTNESS <5-255>  - Set display brightness");
            Log.println("SYNC                - Force NTP time synchronization");
            Log.println("WIFI or IP          - Show WiFi connection info");
            Log.println("LDR                 - Show ambient light sensor state");
            Log.println("WEATHER             - Force a weather refetch");
            Log.println("HOLIDAYS            - Show/refresh market holiday calendars");
            Log.println("LOGSHIP             - Show remote log shipping status");
            Log.println("FACTORYRESET        - Erase all settings + WiFi creds, then reboot");
            Log.println("HELP or ?           - Show this help message");
        }
        else if (command.length() > 0) {
            Log.println("Unknown command. Type HELP for available commands.");
        }
    }
}

void showStartupCommands()
{
    Log.println();
    Log.println("=== World Clock Ready ===");
    Log.println("Serial commands available:");
    Log.println("- BRIGHTNESS <5-255> : Set display brightness");
    Log.println("- SYNC               : Force time synchronization");
    Log.println("- WIFI or IP         : Show network information");
    Log.println("- LDR                : Show ambient light sensor state");
    Log.println("- WEATHER            : Force a weather refetch");
    Log.println("- HELP or ?          : Show command help");
    Log.println("Type any command and press Enter");
    Log.println();
    Log.println("Touch screen controls:");
    Log.println("- Tap CENTER of clock : Open settings (timezones, formats, status)");
    Log.println("- Tap LEFT third      : Decrease brightness");
    Log.println("- Tap RIGHT third     : Increase brightness");
    Log.println();
}
