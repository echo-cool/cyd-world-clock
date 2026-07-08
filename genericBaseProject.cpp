// ----------------------------
// Library Defines - Need to be defined before library import
// ----------------------------

#define ESP_DRD_USE_SPIFFS true

// ----------------------------
// Configuration
// ----------------------------

// Preconfigured WiFi credentials live in an untracked "secrets.h" so they are
// never committed to the repository. Copy secrets.h.example to secrets.h and
// fill in your own values. On boot they are tried alongside the credentials
// saved through the setup portal (portal-saved first), with a fallback to the
// unified AP+STA setup portal (setupPortal.cpp) if nothing connects.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing secrets.h - copy secrets.h.example to secrets.h and set your WiFi credentials"
#endif

#define WIFI_CONNECT_TIMEOUT 5000  // per-attempt timeout for the preconfigured WiFi connection
#define WIFI_CONNECT_ATTEMPTS 10   // attempts before falling back to the setup portal

// Cap on the initial blocking NTP sync at boot. Without a cap the device hangs
// forever here on a network with no usable internet (e.g. an un-logged-in
// captive portal), never reaching the main loop that serves the recovery UI.
#define INITIAL_NTP_SYNC_TIMEOUT_S 20

// ----------------------------
// Standard Libraries
// ----------------------------
#include <WiFi.h>
#include <esp_wifi.h> // esp_wifi_get_config - read the portal-saved credentials

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

#include "setupPortal.h" // unified AP+STA captive-login setup portal (replaces the WiFiManager portal)

#include "cheapYellowLCD.h"

#include "holidayService.h" // named public holidays for the zones

#include "marketHolidays.h" // cached/fetched exchange holiday calendars

#include "uiPages.h" // getPosixFallback - offline timezone rules

#include "wifiWatch.h" // runtime WiFi-loss supervision

#include "netCheck.h" // captive-portal detection + MAC-clone

#include "logShipper.h" // remote log push - anchor capture runs on the main loop

#include "wifiCredentials.h" // host-tested saved-vs-built-in ordering (the boot bug)

// Number of seconds after reset during which a
// subseqent reset will be considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

// The double-reset detector instance (declared extern in genericBaseProject.h).
// Created in baseProjectSetup(); the setup portal stops it before rebooting.
DoubleResetDetector *drd = nullptr;

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

/*-------- NTP server pool ----------*/
// ezTime only queries a single server, and its default (pool.ntp.org) is
// slow or unreachable for users in mainland China. Keep a pool that includes
// servers with good connectivity there, and walk it while syncs keep
// failing: the boot wait below steps through it directly, and
// ntpServerService() keeps rotating ahead of ezTime's automatic retries
// until the first real sync lands. Whichever server answered stays selected.
static const char *NTP_SERVERS[] = {
    "pool.ntp.org",     // worldwide anycast pool (ezTime's default)
    "ntp.aliyun.com",   // Alibaba Cloud - fast inside mainland China
    "ntp.tencent.com",  // Tencent Cloud - fast inside mainland China
    "ntp.ntsc.ac.cn",   // National Time Service Center, Chinese Academy of Sciences
    "time.windows.com", // extra global fallback
};
static const int NTP_SERVER_COUNT = sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]);
static int ntpServerIdx = 0;

const char *currentNtpServer()
{
    return NTP_SERVERS[ntpServerIdx];
}

// Point ezTime at the next server in the pool.
static void ntpNextServer()
{
    ntpServerIdx = (ntpServerIdx + 1) % NTP_SERVER_COUNT;
    setServer(NTP_SERVERS[ntpServerIdx]);
    Log.println("NTP: switching to server " + String(NTP_SERVERS[ntpServerIdx]));
}

// While the clock has never truly synced, rotate the NTP server between
// ezTime's automatic retries (every NTP_RETRY = 20s), so consecutive retries
// each hit a different server instead of hammering one that never answers.
// After the first successful sync the rotation stops - the current server
// demonstrably works, and the half-hourly resyncs keep using it.
static void ntpServerService()
{
    if (ntpSyncStatus) return;
    if (WiFi.status() != WL_CONNECTED) return;
    static unsigned long lastRotate = 0;
    if (millis() - lastRotate < (NTP_RETRY + 10) * 1000UL) return;
    lastRotate = millis();
    ntpNextServer();
}

// Short human text for a wl_status_t, for the boot log / boot console.
static const char *wlStatusText(int st)
{
    switch (st)
    {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "network not found";
    case WL_CONNECT_FAILED:  return "join rejected (wrong password?)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:    return "no link";
    case WL_CONNECTED:       return "connected";
    default:                 return "unknown";
    }
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
        Log.println("No saved config found, will use defaults or the setup portal");
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

    // Step 1: Try the known WiFi credentials before falling back to the config
    // portal. The pair saved by the config portal (in the WiFi stack's NVS) is
    // tried first - the user typed that one in on purpose - then the
    // compiled-in secrets.h pair. See the ordering/dedup comment below.
    if (!forceConfig)
    {
        WiFi.mode(WIFI_STA);
        // Present the cloned MAC (if configured) before the first WiFi.begin so
        // a login-required network sees the authorized address from the start.
        applyStaMacOverride();

        // Portal-saved credentials live in the WiFi stack's NVS (not in our
        // SPIFFS config); esp_wifi has just loaded them into RAM as part of
        // WiFi.mode(WIFI_STA) above, so read them straight from there. Empty
        // until the portal has saved a network once.
        //
        // The ordering (saved before built-in, dedup, skip empties) is the
        // actual bug fix and lives in wifiCredentials.cpp so it can be
        // unit-tested on the host: boot used to try only the secrets.h pair
        // and jump to the portal when it failed, so a network saved through
        // the phone was never retried after the post-save reboot - the clock
        // looped "System initializing..." -> portal forever.
        wifi_config_t staConf = {};
        char savedSsid[sizeof(staConf.sta.ssid) + 1] = "";
        char savedPass[sizeof(staConf.sta.password) + 1] = "";
        if (esp_wifi_get_config(WIFI_IF_STA, &staConf) == ESP_OK)
        {
            memcpy(savedSsid, staConf.sta.ssid, sizeof(staConf.sta.ssid));
            savedSsid[sizeof(staConf.sta.ssid)] = 0;
            memcpy(savedPass, staConf.sta.password, sizeof(staConf.sta.password));
            savedPass[sizeof(staConf.sta.password)] = 0;
        }

        WifiCandidate creds[2];
        int credCount = orderWifiCandidates(savedSsid, savedPass,
                                            PRECONFIGURED_SSID, PRECONFIGURED_PASSWORD,
                                            creds, 2);

        if (credCount == 0)
        {
            Log.println("No WiFi credentials known yet (no portal-saved network, "
                        "empty secrets.h) - going to the config portal");
        }

        // Try each known network in turn, saved before built-in, with several
        // attempts each (a single attempt can fail on a fine network - AP busy,
        // weak signal on boot). The saved network is exhausted before the
        // built-in one is touched at all, so on a normal boot (saved network
        // present) WiFi.begin is only ever called with the saved credentials -
        // it never overwrites the portal-saved NVS entry with the built-in one.
        // Only a saved network that truly can't be reached falls through to
        // built-in, where adopting whatever actually connects is what we want.
        for (int c = 0; c < credCount && !wifiConnected && !bootUiPoll(); c++)
        {
            const WifiCandidate &cred = creds[c];
            Log.println(String("Trying WiFi \"") + cred.ssid + "\" (" + cred.source +
                        "), up to " + String(WIFI_CONNECT_ATTEMPTS) + " attempts");

            for (int attempt = 1; attempt <= WIFI_CONNECT_ATTEMPTS &&
                                  !wifiConnected && !bootUiPoll(); attempt++)
            {
                Log.print("  attempt " + String(attempt) + "/" + String(WIFI_CONNECT_ATTEMPTS) + " ");

                // Reset any half-finished connection state from the previous attempt
                WiFi.disconnect();
                delay(100);
                WiFi.begin(cred.ssid, cred.pass);

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

                if (WiFi.status() == WL_CONNECTED)
                {
                    Log.println(" connected");
                    wifiConnected = true;
                }
                else
                {
                    Log.println(String(" failed: ") + wlStatusText(WiFi.status()));
                }
            }
        }

        if (wifiConnected)
        {
            Log.println("WiFi up: \"" + WiFi.SSID() + "\", RSSI " +
                        String(WiFi.RSSI()) + " dBm");
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
            // Nothing we knew about connected. Undo any credentials the failed
            // WiFi.begin() attempts just wrote to NVS. The esp_wifi stack keeps
            // FLASH storage here (WiFi.persistent() is a no-op after init), so
            // *every* begin() - even one that never links - persists its
            // SSID/password. Left in place, a dead built-in/secrets.h SSID (for
            // example an old "YOUR_WIFI_SSID" placeholder from a previous build)
            // is read back by esp_wifi_get_config as a bogus "saved" network on
            // the next boot and burns the whole retry budget before the real
            // fallback is even tried. Restoring the staConf we read before the
            // connect loop keeps a genuine portal-saved network intact across a
            // transient outage while dropping the dead one.
            esp_wifi_set_config(WIFI_IF_STA, &staConf);
            Log.println("All WiFi attempts failed - opening the config portal");
            forceConfig = true;
        }
    }

    // Step 2: preconfigured WiFi failed (or a double-reset forced setup) - open
    // the unified AP+STA setup portal. It keeps one hotspot up through the whole
    // flow (pick network -> connect -> relay any captive login on the same
    // hotspot -> online) and reboots when done. Skipped when the user asked for
    // the settings page.
    if (!wifiConnected && !bootUiSettingsRequested())
    {
        runSetupPortal(forceConfig, projectConfig);
    }

    // Final check to ensure WiFi is connected. Bounded: both paths above are
    // supposed to have us connected by now, so if we're still offline after
    // 30 seconds something is wedged - reboot and run the whole sequence
    // again rather than hanging here forever. A boot Settings tap skips the
    // wait (and the reboot) so the settings page stays reachable offline.
    unsigned long wifiWaitStart = millis();
    int waitTicks = 0;
    if (WiFi.status() != WL_CONNECTED && !bootUiSettingsRequested())
    {
        Log.println("Waiting for the WiFi link (reboot after 30 s without one)");
    }
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
        Log.println("Checking internet reachability...");
        bootUiPoll(); // paint the console before the blocking probe
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
    bootUiPoll();
    setupOTA();

    // Configure NTP sync frequency for production
    setInterval(1800); // Sync every 30 minutes (production setting)
    setDebug(ERROR); // Only show errors, not all debug info

    Log.println("NTP: initial time sync (server: " + String(currentNtpServer()) +
                ", up to " + String(INITIAL_NTP_SYNC_TIMEOUT_S) + " s)...");

    // Bounded wait: the default waitForSync() (timeout 0) loops forever when
    // NTP never completes, which on a login-required network would hang the
    // whole device at boot - before the main loop starts, so the web page and
    // the on-device Wi-Fi login helper would never come up. Time out instead
    // (open-coded so the boot Settings button stays responsive throughout)
    // and let setup() finish; ezTime's background events() keep retrying, so
    // the clock syncs on its own once the network is actually authorized.
    unsigned long ntpStart = millis();
    unsigned long lastNtpAttempt = millis(); // events() below fires the first query
    while (timeStatus() != timeSet &&
           millis() - ntpStart < INITIAL_NTP_SYNC_TIMEOUT_S * 1000UL &&
           !bootUiPoll())
    {
        events(); // drives the pending NTP query
        // A failed query would otherwise sit out ezTime's 20-second retry -
        // as long as the whole boot budget. Step through the server pool
        // instead (each query gives up after NTP_TIMEOUT = 1.5s), so a boot
        // on a network where the default pool is unreachable - typically
        // mainland China - still syncs within the boot window.
        if (timeStatus() != timeSet && WiFi.status() == WL_CONNECTED &&
            millis() - lastNtpAttempt >= 2000)
        {
            ntpNextServer();
            updateNTP();
            lastNtpAttempt = millis();
        }
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
        Log.println("Initial NTP sync complete! (server: " +
                    String(currentNtpServer()) + ")");
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
        UTC.setTime(compileTime());
        Log.println("Clock seeded with firmware build time: " + UTC.dateTime());
    }

    // EEPROM cache slot 4 (slots 0-3 belong to the world-clock zones in
    // ClockLogic.cpp), so this zone survives timezone-server outages too. The
    // cache payload is uppercased, hence the case-insensitive name check.
    Log.println("Loading home timezone " + projectConfig.timeZone + "...");
    bootUiPoll();
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
    // Until the first real sync lands, walk the NTP server pool so ezTime's
    // retries don't keep hammering a server that never answers.
    ntpServerService();
    // Service over-the-air update requests
    handleOTA();
    // Watch the WiFi link: offline indicator, reconnect kicks, self-heal reboot
    wifiWatchService();
    // Schedule the weekly holiday-calendar refresh (fetch runs on core 0)
    marketHolidaysService();
    // Track the zones' local years for the public-holiday tables
    holidaysService();
    // Anchor the log shipper's timestamps once NTP has really synced
    // (ezTime access must stay on this core)
    logShipperService();
}
