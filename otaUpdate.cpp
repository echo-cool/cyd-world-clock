// Firmware-update concerns of the device's web server: ArduinoOTA (espota /
// IDE network port), the on-TFT update progress screens, and the browser
// firmware updater at /update. The web settings pages live in webSettings.cpp
// and the JSON API in webApi.cpp; this file owns the shared WebServer
// instance and webAuthenticate() (exposed to them through webPortal.h) and
// wires all three route sets together in setupOTA().

#include "otaUpdate.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "webPortal.h"      // webServer + webAuthenticate, shared with the pages
#include "webApi.h"         // webApiRegisterRoutes - the JSON API routes
#include "webSettings.h"    // webSettingsRegisterRoutes - the HTML page routes

#include "ClockLogic.h"     // tft, screenWidth/screenHeight, Log
#include "drdGuard.h"       // rebootCleanly - web-triggered reboots
#include "deviceIdentity.h" // device label shown on the /update page
#include "firmwareInfo.h"   // firmwareGitHash
#include "projectConfig.h"  // projectConfig.hostname
#include "uiPages.h"        // switchToScreen - hand the screen back on failure

// OTA_PASSWORD (optional) lives in the untracked secrets.h.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

volatile bool otaInProgress = false;

// The single HTTP server (port 80). webSettings.cpp and webApi.cpp share it
// through the extern declaration in webPortal.h.
WebServer webServer(80);

static int otaLastPct = -1;

// Web-upload state, valid between UPLOAD_FILE_START and the completion handler
static bool webUploadAuthorized = false;
static size_t webUpdateExpectedSize = 0;
static bool webUpdateOk = false;
static String webUpdateError;

/*-------- Shared TFT progress screen ----------*/
// Laid out from screenWidth/screenHeight so the message, bar and percentage
// sit centered on any panel size (320x240 CYD, 480x320 Hosyond alike).

static int otaBarInnerW() { return (screenWidth * 5) / 8; } // 200px on the CYD
static int otaBarH() { return screenHeight / 13 + 4; }      // 22px on the CYD
static int otaBarX() { return (screenWidth - otaBarInnerW() - 4) / 2; }
static int otaBarY() { return screenHeight / 2; }

static void drawOtaScreen(const String &line, uint16_t color)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(line, screenWidth / 2, (screenHeight * 5) / 12);
}

static void drawOtaProgressFrame()
{
    otaLastPct = -1;
    tft.drawRect(otaBarX(), otaBarY(), otaBarInnerW() + 4, otaBarH(), TFT_WHITE);
}

static void drawOtaProgressPct(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == otaLastPct) return;
    otaLastPct = pct;
    tft.fillRect(otaBarX() + 2, otaBarY() + 2, (otaBarInnerW() * pct) / 100,
                 otaBarH() - 4, TFT_GREEN);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String(pct) + " %  ", screenWidth / 2,
                   otaBarY() + otaBarH() + 8);
}

// Failure exit shared by both update paths: show the error, then hand the
// screen back to the clock with a full repaint.
static void otaFailScreen(const String &line)
{
    otaInProgress = false;
    drawOtaScreen(line, TFT_RED);
    delay(2000);
    switchToScreen(SCREEN_HOME);
}

/*-------- ArduinoOTA (espota / IDE network port) ----------*/

static void setupArduinoOTA()
{
    // Configurable so two clocks on one network don't collide on
    // "<hostname>.local" (web settings page; applied on boot).
    ArduinoOTA.setHostname(projectConfig.hostname.c_str());
#ifdef OTA_PASSWORD
    if (strlen(OTA_PASSWORD) > 0)
    {
        ArduinoOTA.setPassword(OTA_PASSWORD);
    }
#endif

    ArduinoOTA.onStart([]() {
        otaInProgress = true;
        drawOtaScreen(ArduinoOTA.getCommand() == U_FLASH
                          ? "OTA update: receiving firmware..."
                          : "OTA update: receiving filesystem...",
                      TFT_CYAN);
        drawOtaProgressFrame();
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        drawOtaProgressPct(total > 0 ? (int)((progress * 100ULL) / total) : 0);
    });

    ArduinoOTA.onEnd([]() {
        drawOtaScreen("OTA update complete - rebooting...", TFT_GREEN);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        const char *msg = error == OTA_AUTH_ERROR      ? "auth failed"
                          : error == OTA_BEGIN_ERROR   ? "begin failed"
                          : error == OTA_CONNECT_ERROR ? "connect failed"
                          : error == OTA_RECEIVE_ERROR ? "receive failed"
                                                       : "end failed";
        Log.println(String("OTA error: ") + msg);
        otaFailScreen(String("OTA update failed (") + msg + ")");
    });

    ArduinoOTA.begin();
}

/*-------- Web updater (browser firmware upload) ----------*/

// Self-contained page: file picker, browser-side progress bar, result text.
// %BUILD% / %GIT% are replaced with firmware metadata when served.
static const char UPDATE_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>World Clock firmware update</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;display:flex;justify-content:center;padding:2rem}
.card{max-width:26rem;width:100%;background:#1c1c1c;border:1px solid #333;border-radius:10px;padding:1.5rem}
h1{font-size:1.2rem;margin:0 0 .3rem}
p{color:#aaa;font-size:.85rem;margin:.4rem 0}
code{color:#ccc}
input[type=file]{width:100%;margin:.8rem 0;color:#ccc}
button{width:100%;padding:.6rem;border:0;border-radius:6px;background:#0a84ff;color:#fff;font-size:1rem;cursor:pointer}
button:disabled{background:#444;cursor:default}
#bar{height:10px;background:#333;border-radius:5px;overflow:hidden;margin:1rem 0 .4rem;display:none}
#fill{height:100%;width:0;background:#30d158;transition:width .15s}
#msg{font-size:.9rem;min-height:1.2em}
.err{color:#ff6961}.ok{color:#30d158}
</style></head><body><div class="card">
<h1>ESP32 World Clock</h1>
<p>Device: %DEVICE% &middot; MAC %MAC%</p>
<p>Running build: %BUILD% &middot; git %GIT% &middot; <a href="/" style="color:#0a84ff">Settings</a></p>
<p>Select a firmware image (<code>firmware.bin</code> from PlatformIO's
<code>.pio/build/cyd/</code>, or Arduino IDE &gt; Sketch &gt; Export Compiled
Binary) and press Update. Keep the device powered until it reboots.</p>
<form id="f">
<input type="file" id="file" accept=".bin" required>
<button id="btn" type="submit">Update firmware</button>
</form>
<div id="bar"><div id="fill"></div></div>
<div id="msg"></div>
</div>
<script>
var f=document.getElementById('f'),file=document.getElementById('file'),
btn=document.getElementById('btn'),bar=document.getElementById('bar'),
fill=document.getElementById('fill'),msg=document.getElementById('msg');
f.addEventListener('submit',function(e){e.preventDefault();
var fw=file.files[0];if(!fw)return;
btn.disabled=true;bar.style.display='block';fill.style.width='0';
msg.textContent='Uploading...';msg.className='';
var x=new XMLHttpRequest();
x.open('POST','/update?size='+fw.size);
x.upload.onprogress=function(ev){if(ev.lengthComputable)
fill.style.width=Math.round(ev.loaded*100/ev.total)+'%';};
x.onload=function(){if(x.status==200){
msg.textContent='Success - the clock is rebooting; give it ~20 seconds.';
msg.className='ok';}else{
msg.textContent='Update failed: '+x.responseText;
msg.className='err';btn.disabled=false;}};
x.onerror=function(){msg.textContent='Connection lost.';btn.disabled=false;};
var d=new FormData();d.append('firmware',fw);x.send(d);});
</script></body></html>
)rawliteral";

// True if the request may proceed; otherwise a 401 challenge has been sent.
bool webAuthenticate()
{
#ifdef OTA_PASSWORD
    if (strlen(OTA_PASSWORD) > 0 && !webServer.authenticate("admin", OTA_PASSWORD))
    {
        webServer.requestAuthentication();
        return false;
    }
#endif
    return true;
}

static void handleUpdatePage()
{
    if (!webAuthenticate()) return;
    String page = FPSTR(UPDATE_PAGE);
    page.replace("%BUILD%", String(__DATE__) + " " + __TIME__);
    page.replace("%GIT%", firmwareGitHash());
    page.replace("%DEVICE%", deviceLabel());
    page.replace("%MAC%", deviceMacAddress());
    webServer.send(200, "text/html", page);
}

// Streams the multipart upload into the OTA partition chunk by chunk. Runs on
// the main loop core (webServer.handleClient), so drawing on the TFT is safe;
// the clock is intentionally frozen behind the progress screen meanwhile.
static void handleUpdateUpload()
{
    HTTPUpload &up = webServer.upload();

    if (up.status == UPLOAD_FILE_START)
    {
        webUploadAuthorized = true;
#ifdef OTA_PASSWORD
        if (strlen(OTA_PASSWORD) > 0)
        {
            webUploadAuthorized = webServer.authenticate("admin", OTA_PASSWORD);
        }
#endif
        if (!webUploadAuthorized) return;

        otaInProgress = true;
        webUpdateOk = false;
        webUpdateError = "";
        // The page passes the exact file size as ?size= so the on-screen
        // percentage is accurate (multipart Content-Length would overshoot).
        webUpdateExpectedSize = (size_t)webServer.arg("size").toInt();

        Log.println("Web update started: " + up.filename);
        drawOtaScreen("Web update: receiving firmware...", TFT_CYAN);
        drawOtaProgressFrame();

        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
        {
            webUpdateError = Update.errorString();
        }
    }
    else if (up.status == UPLOAD_FILE_WRITE)
    {
        if (!webUploadAuthorized || webUpdateError.length() > 0) return;
        if (Update.write(up.buf, up.currentSize) != up.currentSize)
        {
            webUpdateError = Update.errorString();
            // Close the Update session, or every later Update.begin() fails
            // with "already running" until the device is rebooted.
            Update.abort();
        }
        else if (webUpdateExpectedSize > 0)
        {
            drawOtaProgressPct((int)((up.totalSize * 100ULL) / webUpdateExpectedSize));
        }
    }
    else if (up.status == UPLOAD_FILE_END)
    {
        if (!webUploadAuthorized) return;
        if (webUpdateError.length() == 0 && Update.end(true))
        {
            webUpdateOk = true;
            Log.println("Web update received: " + String(up.totalSize) + " bytes");
        }
        else if (webUpdateError.length() == 0)
        {
            webUpdateError = Update.errorString();
        }
    }
    else if (up.status == UPLOAD_FILE_ABORTED)
    {
        if (!webUploadAuthorized) return;
        Update.abort();
        webUpdateError = "upload aborted";
        // The WebServer never runs the completion handler after an abort, so
        // clean up here: otherwise otaInProgress stays true forever, which
        // silently disables weather/holiday fetches, log shipping and the
        // WiFi self-heal reboot until someone power-cycles the clock.
        Log.println("Web update aborted by client");
        otaFailScreen("Web update aborted");
    }
}

// Completion handler: runs after the upload callback has seen the whole body.
static void handleUpdateResult()
{
    if (!webUploadAuthorized)
    {
        webServer.requestAuthentication();
        return;
    }

    if (webUpdateOk)
    {
        webServer.sendHeader("Connection", "close");
        webServer.send(200, "text/plain", "OK - rebooting");
        drawOtaScreen("Web update complete - rebooting...", TFT_GREEN);
        rebootCleanly(750); // 750 ms lets the response reach the browser
    }
    else
    {
        String err = webUpdateError.length() > 0 ? webUpdateError : "update failed";
        webServer.send(500, "text/plain", err);
        Log.println("Web update failed: " + err);
        otaFailScreen("Web update failed");
    }
}

static void setupWebUpdater()
{
    webSettingsRegisterRoutes(webServer); // "/", /settings, /logs, /wifi-login
    webApiRegisterRoutes(webServer);      // /api/*, /screenshot
    webServer.on("/update", HTTP_GET, handleUpdatePage);
    webServer.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);
    webServer.onNotFound([]() {
        webServer.sendHeader("Location", "/");
        webServer.send(302, "text/plain", "");
    });
    webServer.begin();

    // ArduinoOTA.begin() already registered the configured hostname on mDNS;
    // advertise the web pages on it too.
    MDNS.addService("http", "tcp", 80);
}

/*-------- Public entry points ----------*/

void setupOTA()
{
    setupArduinoOTA();
    setupWebUpdater();
    Log.println("OTA updates enabled (hostname: " + projectConfig.hostname +
                   ", espota port 3232)");
    Log.println("Web settings + updater: http://" + WiFi.localIP().toString() +
                   "/ (or http://" + projectConfig.hostname + ".local/)");
}

void handleOTA()
{
    ArduinoOTA.handle();
    webServer.handleClient();
}
