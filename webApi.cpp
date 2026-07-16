// Machine-facing HTTP endpoints (the JSON API) of the device's web server
// (webPortal.h): /api/status, /api/config backup/restore, /api/factory-reset,
// /api/countdown, /api/logs, and the /screenshot + /api/screen + /api/touch
// debug endpoints. Split out of otaUpdate.cpp - the HTML pages live in
// webSettings.cpp and the firmware updater stays in otaUpdate.cpp.

#include "webApi.h"

#include <ArduinoJson.h>
#include <SPIFFS.h>       // filesystem usage in /api/status
#include <WebServer.h>
#include <WiFi.h>
#include <soc/soc_caps.h> // SOC_TEMP_SENSOR_SUPPORTED
#include <stdlib.h>

#include "webPortal.h"          // webServer, webAuthenticate, otaInProgress

#include "ClockLogic.h"         // tft, backlightLevel, worldZones, ...
#include "drdGuard.h"           // rebootCleanly - web-triggered reboots
#include "clockFaces.h"         // clockFaceName - /api/status
#include "deviceIdentity.h"     // device label shown in status JSON
#include "factoryReset.h"       // factoryReset - /api/factory-reset
#include "firmwareInfo.h"       // firmwareGitHash
#include "genericBaseProject.h" // NTP sync counters - /api/status
#include "holidayService.h"     // holidayZonesLoaded - /api/status
#include "marketHolidays.h"     // marketHolidaysFetchedInfo - /api/status
#include "netCheck.h"           // netReachability - /api/status
#include "timerFaces.h"         // timer state for /api/status + /api/countdown
#include "uiPages.h"            // resetReasonText, uiOpenScreenByName
#include "weatherService.h"     // weatherAgeMinutes
#include "wifiWatch.h"          // outage history - /api/status

/*-------- Config backup / restore ----------*/
// GET /api/config downloads the settings JSON; POST the same JSON back to
// restore it (clone a second device, or recover after a partition-scheme
// change wipes SPIFFS). The device saves the imported config and reboots so
// zones, hostname and formats all apply through the normal boot path.

static void handleApiConfigGet()
{
    if (!webAuthenticate()) return;
    webServer.sendHeader("Content-Disposition",
                         "attachment; filename=\"worldclock-config.json\"");
    webServer.send(200, "application/json", projectConfig.toJsonString());
}

static void handleApiConfigPost()
{
    if (!webAuthenticate()) return;

    String body = webServer.arg("plain");
    if (body.length() == 0 || body.length() > 4096)
    {
        webServer.send(400, "text/plain",
                       "Expected the config JSON (a /api/config backup) as the request body");
        return;
    }
    if (!projectConfig.applyFromJsonString(body))
    {
        webServer.send(400, "text/plain",
                       "Not a valid config JSON - no recognized settings found");
        return;
    }
    if (!projectConfig.saveConfigFile())
    {
        // Without a successful save the imported settings would evaporate on
        // the reboot below - report the failure instead of pretending.
        Log.println("Config import failed: could not persist to SPIFFS");
        webServer.send(500, "text/plain",
                       "Config could not be saved to flash - not rebooting");
        return;
    }
    Log.println("Config imported via /api/config - rebooting to apply");
    webServer.sendHeader("Connection", "close");
    webServer.send(200, "text/plain", "OK - config saved, rebooting to apply it");
    rebootCleanly(750); // 750 ms lets the response reach the client
}

// POST /api/factory-reset wipes every saved setting and WiFi credential and
// reboots to a first-boot state (see factoryReset.h). Handy for re-testing the
// out-of-box flow. Reply first, then erase - factoryReset() takes the radio
// down and never returns.
static void handleApiFactoryReset()
{
    if (!webAuthenticate()) return;
    Log.println("Factory reset requested via /api/factory-reset");
    webServer.sendHeader("Connection", "close");
    webServer.send(200, "text/plain",
                   "OK - erasing all settings and WiFi credentials, rebooting");
    delay(750); // let the response reach the client before the radio drops
    factoryReset();
}

// POST /api/countdown with action=start|pause|resume|reset. Start also takes
// durationSec (60..359999). Commands run on the main loop, the same context as
// physical touch controls, so there is no cross-core timer mutation.
static void sendCountdownResult(int status, const char *message, const char *error = nullptr)
{
    DynamicJsonDocument doc(384);
    doc["ok"] = status < 400;
    if (message) doc["message"] = message;
    if (error) doc["error"] = error;
    JsonObject timer = doc.createNestedObject("countdown");
    timer["state"] = countdownStateName();
    timer["configuredSec"] = countdownConfiguredSec();
    timer["remainingSec"] = countdownRemainingSec();
    String out;
    serializeJson(doc, out);
    webServer.send(status, "application/json", out);
}

static void handleApiCountdown()
{
    if (!webAuthenticate()) return;
    String action = webServer.arg("action");
    action.toLowerCase();

    if (action == "start")
    {
        if (!webServer.hasArg("durationSec"))
        {
            sendCountdownResult(400, nullptr, "durationSec is required");
            return;
        }
        long durationSec = webServer.arg("durationSec").toInt();
        if (durationSec < 60 || durationSec > 359999)
        {
            sendCountdownResult(400, nullptr,
                                "duration must be between 00:01:00 and 99:59:59");
            return;
        }
        if (!countdownWebStart((uint32_t)durationSec))
        {
            sendCountdownResult(409, nullptr,
                                "reset the active countdown before starting a new one");
            return;
        }
        sendCountdownResult(200, "Countdown started");
        return;
    }
    if (action == "pause")
    {
        if (!countdownWebPause())
        {
            sendCountdownResult(409, nullptr, "countdown is not running");
            return;
        }
        sendCountdownResult(200, "Countdown paused");
        return;
    }
    if (action == "resume")
    {
        if (!countdownWebResume())
        {
            sendCountdownResult(409, nullptr, "countdown is not paused");
            return;
        }
        sendCountdownResult(200, "Countdown resumed");
        return;
    }
    if (action == "reset")
    {
        countdownWebReset();
        sendCountdownResult(200, "Countdown reset");
        return;
    }
    sendCountdownResult(400, nullptr, "action must be start, pause, resume, or reset");
}

static void handleApiCountdownStatus()
{
    if (!webAuthenticate()) return;
    sendCountdownResult(200, nullptr);
}

// Diagnostics as JSON - the System status page, but scriptable.
static void handleApiStatus()
{
    if (!webAuthenticate()) return;

    DynamicJsonDocument doc(5120);
    doc["hostname"] = projectConfig.hostname;
    doc["ip"] = WiFi.localIP().toString();
    doc["ssid"] = WiFi.SSID();
    doc["rssiDbm"] = WiFi.RSSI();
    doc["mac"] = WiFi.macAddress();
    if (projectConfig.staMacOverride.length() > 0)
    {
        doc["macCloned"] = true;
    }
    switch (netReachability())
    {
    case NET_ONLINE: doc["internet"] = "online"; break;
    case NET_CAPTIVE: doc["internet"] = "captive"; break;
    default: doc["internet"] = "offline"; break;
    }
    doc["gateway"] = WiFi.gatewayIP().toString();
    doc["dns"] = WiFi.dnsIP().toString();
    doc["wifiChannel"] = WiFi.channel();
    doc["wifiDrops"] = wifiDropCount();
    doc["wifiOfflineSec"] = (long)(wifiOfflineDurationMs() / 1000UL);
    doc["resetReason"] = resetReasonText();
    doc["sdk"] = ESP.getSdkVersion();
    doc["spiffsUsedBytes"] = SPIFFS.usedBytes();
    doc["spiffsTotalBytes"] = SPIFFS.totalBytes();
    doc["chip"] = String(ESP.getChipModel()) + " r" + String(ESP.getChipRevision());
    doc["cpuMhz"] = ESP.getCpuFreqMHz();
#if SOC_TEMP_SENSOR_SUPPORTED
    doc["cpuTempC"] = temperatureRead();
#endif
    doc["flashSizeBytes"] = ESP.getFlashChipSize();
    doc["sketchSizeBytes"] = ESP.getSketchSize();
    // getFreeSketchSpace() is the OTA slot's capacity (see fillSystemValues)
    doc["sketchSlotBytes"] = ESP.getFreeSketchSpace();
    doc["uptimeSec"] = millis() / 1000UL;
    doc["freeHeapBytes"] = ESP.getFreeHeap();
    doc["minFreeHeapBytes"] = ESP.getMinFreeHeap();
    doc["maxAllocHeapBytes"] = ESP.getMaxAllocHeap();
    doc["ntpSyncs"] = syncCount;
    doc["lastSyncAgoMin"] = (syncCount > 0)
                                ? (long)((millis() - lastSyncTime) / 60000UL)
                                : -1;
    doc["ntpServer"] = currentNtpServer();
    doc["utc"] = UTC.dateTime("Y-m-d H:i:s");
    doc["utcEpoch"] = (long)UTC.now();
    doc["build"] = String(__DATE__) + " " + __TIME__;
    doc["git"] = firmwareGitHash();
    doc["device"] = deviceLabel();
    doc["deviceMac"] = deviceMacAddress();
    doc["board"] = BOARD_PROFILE_NAME;
    doc["displayWidth"] = tft.width();
    doc["displayHeight"] = tft.height();
    doc["clockFace"] = clockFaceName(projectConfig.clockFace);
    doc["brightness"] = backlightLevel;
    doc["weatherAgeMin"] = weatherAgeMinutes();

    // Timer faces (sessions are in-RAM only; they reset on reboot)
    doc["timerReminderMin"] = projectConfig.timerReminderMin;
    doc["timerHideSeconds"] = projectConfig.timerHideSeconds;
    JsonObject swj = doc.createNestedObject("stopwatch");
    swj["state"] = stopwatchStateName();
    swj["elapsedSec"] = stopwatchElapsedSec();
    JsonObject cdj = doc.createNestedObject("countdown");
    cdj["state"] = countdownStateName();
    cdj["configuredSec"] = countdownConfiguredSec();
    cdj["remainingSec"] = countdownRemainingSec();

    long calAgeDays = -1;
    doc["marketHolidaySource"] = marketHolidaysFetchedInfo(calAgeDays) ? "fetched" : "compiled";
    if (calAgeDays >= 0)
    {
        doc["marketHolidayAgeDays"] = calAgeDays;
    }
    int holidayZonesEligible = 0;
    doc["holidayZonesLoaded"] = holidayZonesLoaded(holidayZonesEligible);
    doc["holidayZonesEligible"] = holidayZonesEligible;

    doc["autoBrightness"] = projectConfig.autoBrightness;
    bool ldrTrusted, ldrDark;
    int ldrSmoothed;
    if (getLdrState(ldrTrusted, ldrDark, ldrSmoothed))
    {
        doc["ldrTrusted"] = ldrTrusted;
        if (ldrTrusted)
        {
            doc["ldrRoomDark"] = ldrDark;
        }
    }
    doc["manualBrightnessHoldMin"] = (long)(manualBrightnessRemainingMs() / 60000UL);

    JsonArray zones = doc.createNestedArray("zones");
    for (int i = 0; i < 4; i++)
    {
        JsonObject z = zones.createNestedObject();
        z["name"] = worldZones[i].name;
        z["tz"] = worldZones[i].timezone;
        time_t local = worldZones[i].tz.now();
        z["localTime"] = worldZones[i].tz.dateTime("Y-m-d H:i:s T P");
        z["localEpoch"] = (long)local;
        z["utcOffsetMin"] = -worldZones[i].tz.getOffset();
        z["olson"] = worldZones[i].tz.getOlson();
        z["posix"] = worldZones[i].tz.getPosix();
        ZoneWeather w = getZoneWeather(i);
        if (w.valid)
        {
            z["temp"] = displayTemp(w.tempC);
            z["tempUnit"] = String(tempUnitLetter());
            z["weatherCode"] = w.weatherCode;
            z["weather"] = weatherCodeText(w.weatherCode);
            String notice = getZonePrecipNotice(i);
            if (notice.length() > 0)
            {
                z["weatherNotice"] = notice;
            }
        }
        if (worldZones[i].lastMarketStatus.length() > 0)
        {
            z["market"] = worldZones[i].lastMarketStatus;
        }
    }

    String out;
    serializeJson(doc, out);
    webServer.send(200, "application/json", out);
}

/*-------- Log tail ----------*/
// GET /api/logs - the tail of the in-RAM log ring (logBuffer.h) as plain
// text. The human-facing auto-refreshing viewer at /logs lives in
// webSettings.cpp.

static void handleApiLogs()
{
    if (!webAuthenticate()) return;
    webServer.send(200, "text/plain", logTail(6144));
}

/*-------- Screenshot (debug) ----------*/
// GET /screenshot streams the panel's current contents as a 24-bit BMP,
// read back from the display controller over SPI one
// line at a time. Lets a developer see exactly what a remote clock is
// showing (bug reports, UI work) without standing in front of it. Runs on
// the main loop core between frame updates like every other handler, so the
// clock UI just pauses for the ~1-2 s transfer.

static void handleScreenshot()
{
    if (!webAuthenticate()) return;
    if (otaInProgress)
    {
        webServer.send(503, "text/plain", "update in progress");
        return;
    }

    const int w = tft.width();
    const int h = tft.height();
    const uint32_t rowBytes = (uint32_t)w * 3; // 24bpp
    const uint32_t imgBytes = rowBytes * h;
    const uint32_t fileSize = 54 + imgBytes;

    uint16_t *line = (uint16_t *)malloc((size_t)w * sizeof(uint16_t));
    uint8_t *row = (uint8_t *)malloc((size_t)rowBytes);
    if (!line || !row)
    {
        free(line);
        free(row);
        webServer.send(503, "text/plain", "not enough heap for screenshot");
        return;
    }

    // BITMAPFILEHEADER + BITMAPINFOHEADER, little-endian
    uint8_t hdr[54] = {0};
    auto put32 = [&hdr](int off, uint32_t v) {
        hdr[off] = v & 0xFF;
        hdr[off + 1] = (v >> 8) & 0xFF;
        hdr[off + 2] = (v >> 16) & 0xFF;
        hdr[off + 3] = (v >> 24) & 0xFF;
    };
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(2, fileSize);
    hdr[10] = 54; // pixel data offset
    hdr[14] = 40; // BITMAPINFOHEADER size
    put32(18, (uint32_t)w);
    put32(22, (uint32_t)h);
    hdr[26] = 1;  // planes
    hdr[28] = 24; // bits per pixel, BI_RGB
    put32(34, imgBytes);

    webServer.sendHeader("Cache-Control", "no-store");
    webServer.sendHeader("Content-Disposition", "inline; filename=\"worldclock.bmp\"");
    webServer.setContentLength(fileSize);
    webServer.send(200, "image/bmp", "");
    webServer.sendContent((const char *)hdr, sizeof(hdr));

    for (int y = h - 1; y >= 0; y--) // BMP pixel rows run bottom-up
    {
        tft.readRect(0, y, w, 1, line);
        for (int x = 0; x < w; x++)
        {
            // The CYD's ILI9341 clone returns its 18-bit RAMRD stream shifted
            // by one channel, so the field the driver stores as red actually
            // carries the drawn GREEN, green carries BLUE and blue carries
            // RED (verified by sampling known TFT_BLUE / GREEN / ORANGE /
            // YELLOW screen elements; greys are unaffected). Rotate the
            // channels back while packing the BGR888 BMP row.
            uint16_t c = line[x];
            row[x * 3 + 0] = ((c >> 5) & 0x3F) << 2; // BMP blue  <- "G" field
            row[x * 3 + 1] = (c >> 11) << 3;         // BMP green <- "R" field
            row[x * 3 + 2] = (c & 0x1F) << 3;        // BMP red   <- "B" field
        }
        webServer.sendContent((const char *)row, rowBytes);
    }
    free(line);
    free(row);
}

/*-------- Remote UI navigation (debug) ----------*/
// GET/POST /api/screen?name=<page>[&page=N][&slot=N] switches the on-device
// UI to the named page (home, settings, zones, tzlist, status, logs,
// wifilogin), exactly as tapping through the touch UI would; without ?name
// it just reports the page currently showing. Paired with /screenshot this
// lets a developer capture and review any UI page remotely.

static void handleApiScreen()
{
    if (!webAuthenticate()) return;

    String name = webServer.arg("name");
    if (name.length() > 0)
    {
        int page = webServer.arg("page").toInt(); // 0 when absent
        int slot = webServer.arg("slot").toInt();
        if (!uiOpenScreenByName(name, page, slot))
        {
            webServer.send(400, "text/plain",
                           "unknown page - use one of: home, settings, zones, "
                           "tzlist, status, logs, wifilogin, wififail, caltouch "
                           "(caltouch only on boards with TFT_eSPI touch)");
            return;
        }
        Log.println("UI page \"" + name + "\" opened via /api/screen");
    }
    webServer.send(200, "application/json",
                   String("{\"screen\":\"") + uiScreenName() + "\"}");
}

/*-------- Remote touch injection (debug) ----------*/
// GET/POST /api/touch?x=<px>&y=<px>[&ms=<hold>] simulates a finger tap at the
// given screen pixels: readTouchPoint() reports the press for the hold time
// (default 100ms, 20-2000), then a release. It flows through the exact same
// paths as a physical touch - home-screen zones, the timer faces' buttons,
// UI page buttons, touch suppression, alarm dismissal - so together with
// /screenshot the whole touch UI can be exercised remotely: look, tap what
// you see, look again. Coordinates match the screenshot (the 180-degree
// display flip is already accounted for).

static void handleApiTouch()
{
    if (!webAuthenticate()) return;
    if (otaInProgress)
    {
        webServer.send(503, "text/plain", "update in progress");
        return;
    }
    if (!webServer.hasArg("x") || !webServer.hasArg("y"))
    {
        webServer.send(400, "text/plain",
                       "usage: /api/touch?x=<px>&y=<px>[&ms=<hold, 20-2000>]");
        return;
    }

    int x = webServer.arg("x").toInt();
    int y = webServer.arg("y").toInt();
    if (x < 0 || x >= tft.width() || y < 0 || y >= tft.height())
    {
        webServer.send(400, "text/plain",
                       "coordinates outside the display (" + String(tft.width()) +
                           "x" + String(tft.height()) + ")");
        return;
    }
    long holdMs = webServer.hasArg("ms") ? webServer.arg("ms").toInt() : 100;
    holdMs = constrain(holdMs, 20, 2000);

    injectTouchPoint(x, y, (unsigned long)holdMs);
    Log.println("Synthetic touch via /api/touch at " + String(x) + "," +
                String(y) + " (" + String(holdMs) + "ms)");
    webServer.send(200, "application/json",
                   "{\"ok\":true,\"x\":" + String(x) + ",\"y\":" + String(y) +
                       ",\"ms\":" + String(holdMs) + ",\"screen\":\"" +
                       uiScreenName() + "\"}");
}

/*-------- Route registration ----------*/

// Register the JSON API + debug endpoints on the shared web server. Called
// once from setupOTA() (otaUpdate.cpp) before webServer.begin().
void webApiRegisterRoutes(WebServer &server)
{
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/countdown", HTTP_GET, handleApiCountdownStatus);
    server.on("/api/countdown", HTTP_POST, handleApiCountdown);
    server.on("/api/config", HTTP_GET, handleApiConfigGet);
    server.on("/api/config", HTTP_POST, handleApiConfigPost);
    server.on("/api/factory-reset", HTTP_POST, handleApiFactoryReset);
    server.on("/api/logs", HTTP_GET, handleApiLogs);
    server.on("/screenshot", HTTP_GET, handleScreenshot);
    server.on("/api/screen", handleApiScreen);
    server.on("/api/touch", handleApiTouch);
}
