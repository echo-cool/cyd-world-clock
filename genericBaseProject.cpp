// ----------------------------
// Library Defines - Need to be defined before library import
// ----------------------------

#define ESP_DRD_USE_SPIFFS true

// ----------------------------
// Configuration
// ----------------------------

// Preconfigured WiFi credentials live in an untracked "secrets.h" so they are
// never committed to the repository. Copy secrets.h.example to secrets.h and
// fill in your own values. These are tried first on boot, with a fallback to
// the WiFiManager captive portal if the connection fails.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing secrets.h - copy secrets.h.example to secrets.h and set your WiFi credentials"
#endif

#define WIFI_CONNECT_TIMEOUT 5000  // per-attempt timeout for the preconfigured WiFi connection
#define WIFI_CONNECT_ATTEMPTS 10   // attempts before falling back to the WiFiManager portal

// Cap on the initial blocking NTP sync at boot. Without a cap the device hangs
// forever here on a network with no usable internet (e.g. an un-logged-in
// captive portal), never reaching the main loop that serves the recovery UI.
#define INITIAL_NTP_SYNC_TIMEOUT_S 20

// ----------------------------
// Standard Libraries
// ----------------------------
#include <WiFi.h>

#include <FS.h>
#include "SPIFFS.h"

// ----------------------------
// Additional Libraries - each one of these will need to be installed.
// ----------------------------

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

// ----------------------------
// Internal includes
// ----------------------------

#include "genericBaseProject.h"

#include "otaUpdate.h"

#include "wifiManagerHandler.h"

#include "cheapYellowLCD.h"

#include "holidayService.h" // named public holidays for the zones

#include "marketHolidays.h" // cached/fetched exchange holiday calendars

#include "uiPages.h" // getPosixFallback - offline timezone rules

#include "wifiWatch.h" // runtime WiFi-loss supervision

#include "netCheck.h" // captive-portal detection + MAC-clone

// Number of seconds after reset during which a
// subseqent reset will be considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

// NTP sync monitoring variables
unsigned long lastSyncTime = 0;
unsigned long syncCount = 0;
bool ntpSyncStatus = false;

CheapYellowDisplay cyd;
ProjectDisplay *projectDisplay = &cyd;

Timezone myTZ;

// Handle ezTime NTP synchronization and sync monitoring.
//
// IMPORTANT: ezTime is not thread-safe. This runs from the main loop (the same
// context that reads the time and calls setLocation()) so that all ezTime
// access happens on a single core. A previous version ran events() in a task
// pinned to Core 0 while the main loop read the clock on Core 1, which is an
// unsynchronized concurrent access to ezTime's internal state.
static void handleTimeSync() {
    static unsigned long lastStatusReport = 0;
    static time_t lastKnownTime = 0;

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

            Log.println("NTP Sync #" + String(syncCount) + " - " + UTC.dateTime() + " (Uptime: " + String(millis()/1000/60) + "min)");
        }
        lastKnownTime = timeAfter;
    }

    // Report sync status every 30 minutes (production frequency)
    if (millis() - lastStatusReport > 1800000) { // 30 minutes
        lastStatusReport = millis();

        Log.println("NTP Status: " + String(syncCount) + " syncs, Last: " + String((millis() - lastSyncTime)/1000/60) + "min ago");
    }
}

// UTC epoch of this firmware build, parsed from the compiler's __DATE__
// ("Jul  7 2026") and __TIME__ ("12:34:56"). Used to seed the clock when NTP
// is unreachable; build-machine local time is close enough for a placeholder.
static time_t firmwareBuildEpoch()
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char monStr[4] = {__DATE__[0], __DATE__[1], __DATE__[2], '\0'};
    const char *m = strstr(months, monStr);
    uint8_t month = m ? (m - months) / 3 + 1 : 1;
    return makeTime(atoi(__TIME__), atoi(__TIME__ + 3), atoi(__TIME__ + 6),
                    atoi(__DATE__ + 4), month, atoi(__DATE__ + 7));
}

void baseProjectSetup()
{
    // SPIFFS and the saved config come up before the display so settings that
    // affect the panel itself (flipDisplay) apply from the very first pixel.
    bool spiffsInitSuccess = SPIFFS.begin(false) || SPIFFS.begin(true);
    if (!spiffsInitSuccess)
    {
        Log.println("SPIFFS initialisation failed!");
        while (1)
            yield(); // Stay here twiddling thumbs waiting
    }
    Log.println("\r\nInitialisation done.");

    // Try to load existing config first
    if (!projectConfig.fetchConfigFile())
    {
        Log.println("No saved config found, will use defaults or WiFiManager");
    }

    // Load the cached market holiday calendars (SPIFFS is up); the weekly
    // network refresh is scheduled later from the main loop.
    marketHolidaysBegin();

    projectDisplay->displaySetup();

    // Give the init screen a Settings button: the wait loops below poll it,
    // and a tap cuts the remaining network waits short so the main loop can
    // start directly on the settings page (Wi-Fi login helper, status, logs).
    bootUiBegin();

    bool forceConfig = false;
    bool wifiConnected = false;

    // Initialize double reset detector
    drd = new DoubleResetDetector(DRD_TIMEOUT, DRD_ADDRESS);
    if (drd->detectDoubleReset())
    {
        Log.println(F("Forcing config mode as there was a Double reset detected"));
        forceConfig = true;
    }

    // Step 1: Try preconfigured WiFi credentials first. A single attempt can
    // fail even when the network is fine (AP busy, weak signal on boot), so
    // retry several times before falling back to the config portal.
    if (!forceConfig)
    {
        Log.println("Attempting to connect with preconfigured WiFi...");
        WiFi.mode(WIFI_STA);
        // Present the cloned MAC (if configured) before the first WiFi.begin so
        // a login-required network sees the authorized address from the start.
        applyStaMacOverride();

        for (int attempt = 1; attempt <= WIFI_CONNECT_ATTEMPTS && !wifiConnected && !bootUiPoll(); attempt++)
        {
            Log.print("WiFi connect attempt ");
            Log.print(attempt);
            Log.print("/");
            Log.print(WIFI_CONNECT_ATTEMPTS);
            Log.print(" ");

            // Reset any half-finished connection state from the previous attempt
            WiFi.disconnect();
            delay(100);
            WiFi.begin(PRECONFIGURED_SSID, PRECONFIGURED_PASSWORD);

            // Poll every 50ms so a quick tap on the boot Settings button is
            // never missed; keep the progress dots at their ~500ms cadence.
            unsigned long startTime = millis();
            int ticks = 0;
            while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_CONNECT_TIMEOUT && !bootUiPoll())
            {
                delay(50);
                if (++ticks % 10 == 0)
                {
                    Log.print(".");
                }
            }
            Log.println();

            if (WiFi.status() == WL_CONNECTED)
            {
                wifiConnected = true;
            }
        }

        if (wifiConnected)
        {
            Log.println("Connected with preconfigured WiFi!");
            Log.print("IP address: ");
            Log.println(WiFi.localIP());

            // Use preconfigured settings if no saved config
            if (projectConfig.timeZone.length() == 0)
            {
                projectConfig.timeZone = PRECONFIGURED_TIMEZONE;
                projectConfig.twentyFourHour = false;
                projectConfig.usDateFormat = true;
                Log.println("Using preconfigured timezone and settings");
            }
        }
        else if (bootUiSettingsRequested())
        {
            Log.println("Boot cut short before WiFi connected - the STA keeps "
                        "retrying in the background");
        }
        else
        {
            Log.println("All preconfigured WiFi attempts failed, falling back to WiFiManager");
            forceConfig = true;
        }
    }

    // Step 2: If preconfigured WiFi failed or forced config, use WiFiManager
    // (skipped when the user asked for the settings page - the config portal
    // blocks for minutes and has its own reboot-to-retry recovery).
    if (!wifiConnected && !bootUiSettingsRequested())
    {
        setupWiFiManager(forceConfig, projectConfig, projectDisplay);
    }

    // Final check to ensure WiFi is connected. Bounded: both paths above are
    // supposed to have us connected by now, so if we're still offline after
    // 30 seconds something is wedged - reboot and run the whole sequence
    // again rather than hanging here forever. A boot Settings tap skips the
    // wait (and the reboot) so the settings page stays reachable offline.
    unsigned long wifiWaitStart = millis();
    int waitTicks = 0;
    while (WiFi.status() != WL_CONNECTED && !bootUiPoll())
    {
        if (millis() - wifiWaitStart > 30000UL)
        {
            Log.println("\nStill no WiFi after portal/connect - rebooting to retry");
            drd->stop(); // avoid the reboot registering as a double reset
            delay(1000);
            ESP.restart();
        }
        if (++waitTicks % 10 == 0)
        {
            Log.print(".");
        }
        delay(50);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Log.println("");
        Log.println("WiFi connected");
        Log.print("IP address: ");
        Log.println(WiFi.localIP());

        // Association succeeds even on a login-required (captive-portal) network,
        // where everything past this point (NTP, weather, holidays) would silently
        // fail. Probe once so the reason is in the log; the runtime re-check on the
        // weather task keeps the on-screen / API captive state current thereafter.
        switch (netCheckNow())
        {
        case NET_ONLINE:
            Log.println("Internet check: online");
            break;
        case NET_CAPTIVE:
            Log.println("Internet check: CAPTIVE PORTAL - this network needs a "
                        "browser login. See http://" + WiFi.localIP().toString() +
                        "/wifi-login (NTP/weather stay unavailable until it is "
                        "authorized for MAC " + WiFi.macAddress() + ")");
            break;
        default:
            Log.println("Internet check: no response (network may be offline or "
                        "blocking outbound traffic)");
            break;
        }
    }
    else
    {
        Log.println("");
        Log.println("Continuing to the settings page without WiFi - use the "
                    "WiFi login helper / status / logs from there");
    }

    // Enable over-the-air firmware updates now that the network is up
    setupOTA();

    Log.println("Waiting for time sync");

    // Configure NTP sync frequency for production
    setInterval(1800); // Sync every 30 minutes (production setting)
    setDebug(ERROR); // Only show errors, not all debug info

    Log.println("Performing initial NTP sync...");

    // Bounded wait: the default waitForSync() (timeout 0) loops forever when
    // NTP never completes, which on a login-required network would hang the
    // whole device at boot - before the main loop starts, so the web page and
    // the on-device Wi-Fi login helper would never come up. Time out instead
    // (open-coded so the boot Settings button stays responsive throughout)
    // and let setup() finish; ezTime's background events() keep retrying, so
    // the clock syncs on its own once the network is actually authorized.
    unsigned long ntpStart = millis();
    while (timeStatus() != timeSet &&
           millis() - ntpStart < INITIAL_NTP_SYNC_TIMEOUT_S * 1000UL &&
           !bootUiPoll())
    {
        events(); // drives the pending NTP query
        delay(50);
    }
    if (timeStatus() == timeSet)
    {
        // Count the boot-time sync too - it used to be invisible to the sync
        // counters, so the status pages claimed "none since boot" for the
        // first half hour even though the clock was synced all along.
        syncCount++;
        lastSyncTime = millis();
        ntpSyncStatus = true;
        Log.println("Initial NTP sync complete!");
        Log.println();
        Log.println("UTC:             " + UTC.dateTime());
    }
    else
    {
        Log.println("Initial NTP sync skipped or timed out - continuing so the "
                    "UI / web settings / Wi-Fi login helper are available; the "
                    "clock will sync once it has real internet");

        // Seed the clock with the firmware build time. Unseeded, ezTime starts
        // at the 1970 epoch, and every zone west of UTC then lives at a
        // NEGATIVE local time_t for the first hours of uptime: ezTime's
        // hour()/minute() return a negative remainder through a uint8_t (a
        // Santa Clara afternoon renders as "249:199") and its date math wraps
        // to February 2106. A recent-but-unsynced time keeps all zones
        // positive and mutually consistent; the pending NTP retries correct
        // the clock the moment real internet appears.
        UTC.setTime(firmwareBuildEpoch());
        Log.println("Clock seeded with firmware build time: " + UTC.dateTime());
    }

    // EEPROM cache slot 4 (slots 0-3 belong to the world-clock zones in
    // ClockLogic.cpp), so this zone survives timezone-server outages too. The
    // cache payload is uppercased, hence the case-insensitive name check.
    if (!(myTZ.setCache(4 * EEPROM_CACHE_LEN) &&
          myTZ.getOlson().equalsIgnoreCase(projectConfig.timeZone)))
    {
        // On a boot cut short by the Settings button there is no internet
        // worth waiting on - go straight to the built-in POSIX rules.
        if (bootUiSettingsRequested() || !myTZ.setLocation(projectConfig.timeZone))
        {
            // Timezone server unreachable and nothing cached: preset zones
            // carry built-in POSIX rules so local time is still correct.
            const char *posix = getPosixFallback(projectConfig.timeZone);
            if (posix)
            {
                myTZ.setPosix(posix);
                Log.println("Timezone server skipped/unreachable - using built-in POSIX rules");
            }
        }
    }
    Log.print(projectConfig.timeZone);
    Log.print(F(":     "));
    Log.println(myTZ.dateTime());
    Log.println("-------------------------");
}

void baseProjectLoop()
{
    drd->loop();
    // Handle ezTime NTP events on the main core (ezTime is not thread-safe).
    handleTimeSync();
    // Service over-the-air update requests
    handleOTA();
    // Watch the WiFi link: offline indicator, reconnect kicks, self-heal reboot
    wifiWatchService();
    // Schedule the weekly holiday-calendar refresh (fetch runs on core 0)
    marketHolidaysService();
    // Track the zones' local years for the public-holiday tables
    holidaysService();
}
