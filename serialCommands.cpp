#include "serialCommands.h"

#include <WiFi.h>
#include <ezTime.h>

#include "ClockLogic.h"         // backlightLevel, worldZones, manualBrightnessUntil
#include "genericBaseProject.h" // BACKLIGHT_PIN
#include "holidayService.h"     // public holiday status
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
                    Serial.print("Brightness set to: ");
                    Serial.println(backlightLevel);
                } else {
                    Serial.println("Error: Brightness must be between 5 and 255");
                }
            } else {
                Serial.println("Error: Usage: BRIGHTNESS <value>");
            }
        }
        else if (command == "SYNC") {
            Serial.println("Forcing NTP time synchronization...");
            // Force immediate sync by calling updateNTP manually
            if (waitForSync(10)) { // Wait up to 10 seconds for sync
                Serial.println("Time sync successful!");
                Serial.println("UTC: " + UTC.dateTime());
                Serial.println("Santa Clara: " + worldZones[0].tz.dateTime());
            } else {
                Serial.println("Time sync failed or timed out");
            }
        }
        else if (command == "WIFI" || command == "IP") {
            Serial.println("=== WiFi Information ===");
            Serial.print("SSID: ");
            Serial.println(WiFi.SSID());
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            Serial.print("Gateway: ");
            Serial.println(WiFi.gatewayIP());
            Serial.print("Subnet: ");
            Serial.println(WiFi.subnetMask());
            Serial.print("DNS: ");
            Serial.println(WiFi.dnsIP());
            Serial.print("Signal Strength (RSSI): ");
            Serial.print(WiFi.RSSI());
            Serial.println(" dBm");
            Serial.print("MAC Address: ");
            Serial.println(WiFi.macAddress());
            Serial.print("Connection Status: ");
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("Connected");
            } else {
                Serial.println("Disconnected");
            }
        }
        else if (command == "LDR") {
            printLdrStatus();
        }
        else if (command == "WEATHER") {
            long age = weatherAgeMinutes();
            Serial.println(age < 0 ? "No weather data yet."
                                   : "Weather data is " + String(age) + " min old.");
            weatherInvalidate();
            Serial.println("Cache invalidated - the background task refetches within seconds.");
        }
        else if (command == "HOLIDAYS") {
            printMarketHolidaysStatus();
            marketHolidaysForceRefresh();
            printPublicHolidaysStatus();
        }
        else if (command == "HELP" || command == "?") {
            Serial.println("=== Available Commands ===");
            Serial.println("BRIGHTNESS <5-255>  - Set display brightness");
            Serial.println("SYNC                - Force NTP time synchronization");
            Serial.println("WIFI or IP          - Show WiFi connection info");
            Serial.println("LDR                 - Show ambient light sensor state");
            Serial.println("WEATHER             - Force a weather refetch");
            Serial.println("HOLIDAYS            - Show/refresh market holiday calendars");
            Serial.println("HELP or ?           - Show this help message");
        }
        else if (command.length() > 0) {
            Serial.println("Unknown command. Type HELP for available commands.");
        }
    }
}

void showStartupCommands()
{
    Serial.println();
    Serial.println("=== World Clock Ready ===");
    Serial.println("Serial commands available:");
    Serial.println("- BRIGHTNESS <5-255> : Set display brightness");
    Serial.println("- SYNC               : Force time synchronization");
    Serial.println("- WIFI or IP         : Show network information");
    Serial.println("- LDR                : Show ambient light sensor state");
    Serial.println("- WEATHER            : Force a weather refetch");
    Serial.println("- HELP or ?          : Show command help");
    Serial.println("Type any command and press Enter");
    Serial.println();
    Serial.println("Touch screen controls:");
    Serial.println("- Tap CENTER of clock : Open settings (timezones, formats, status)");
    Serial.println("- Tap LEFT third      : Decrease brightness");
    Serial.println("- Tap RIGHT third     : Increase brightness");
    Serial.println();
}
