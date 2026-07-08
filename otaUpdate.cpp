#include "otaUpdate.h"

#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>       // filesystem usage in /api/status
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <soc/soc_caps.h> // SOC_TEMP_SENSOR_SUPPORTED

#include "ClockLogic.h"         // tft, backlightLevel, SHOW_24HOUR, ...
#include "clockFaces.h"         // FACE_COUNT, clockFaceName
#include "factoryReset.h"       // factoryReset - /api/factory-reset
#include "genericBaseProject.h" // BACKLIGHT_PIN, NTP sync counters
#include "holidayService.h"     // holidayZonesLoaded - /api/status
#include "marketHolidays.h"     // marketHolidaysFetchedInfo - /api/status
#include "netCheck.h"           // captive state, MAC parse - /wifi-login + settings
#include "wifiRelay.h"          // login-relay helper trigger + state
#include "uiPages.h"            // TZ_PRESETS, applyZoneSelection, resetReasonText
#include "weatherService.h"     // weatherAgeMinutes
#include "wifiWatch.h"          // outage history - /api/status

// OTA_PASSWORD (optional) lives in the untracked secrets.h.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

volatile bool otaInProgress = false;

static WebServer webServer(80);

static int otaLastPct = -1;

// Web-upload state, valid between UPLOAD_FILE_START and the completion handler
static bool webUploadAuthorized = false;
static size_t webUpdateExpectedSize = 0;
static bool webUpdateOk = false;
static String webUpdateError;

/*-------- Shared TFT progress screen ----------*/

static void drawOtaScreen(const String &line, uint16_t color)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(line, 160, 100);
}

static void drawOtaProgressFrame()
{
    otaLastPct = -1;
    tft.drawRect(58, 120, 204, 18, TFT_WHITE);
}

static void drawOtaProgressPct(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == otaLastPct) return;
    otaLastPct = pct;
    tft.fillRect(60, 122, pct * 2, 14, TFT_GREEN);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String(pct) + " %  ", 160, 146);
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
// %BUILD% is replaced with the compile timestamp when served.
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
<p>Running build: %BUILD% &middot; <a href="/" style="color:#0a84ff">Settings</a></p>
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
static bool webAuthenticate()
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
        Update.abort();
        webUpdateError = "upload aborted";
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
        delay(750); // let the response reach the browser
        ESP.restart();
    }
    else
    {
        String err = webUpdateError.length() > 0 ? webUpdateError : "update failed";
        webServer.send(500, "text/plain", err);
        Log.println("Web update failed: " + err);
        otaFailScreen("Web update failed");
    }
}

/*-------- Web settings page ----------*/
// Configure the clock from a browser: the same settings as the on-device
// touch UI (timezones, face, formats, brightness). Served at "/"; changes
// are applied on the main loop core (webServer.handleClient runs there), so
// it can safely reuse the touch UI's apply/persist functions.

static const char SETTINGS_PAGE_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>World Clock settings</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;display:flex;justify-content:center;padding:2rem}
.card{max-width:26rem;width:100%;background:#1c1c1c;border:1px solid #333;border-radius:10px;padding:1.5rem}
h1{font-size:1.2rem;margin:0 0 .3rem}
p{color:#aaa;font-size:.85rem;margin:.4rem 0}
a{color:#0a84ff}
label{display:block;color:#ccc;font-size:.85rem;margin:.8rem 0 0}
select,input[type=range],input[type=text]{width:100%;margin:.3rem 0 0;padding:.4rem;background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:6px;box-sizing:border-box}
.row{display:flex;gap:.6rem}.row label{flex:1;margin-top:.8rem}
fieldset{border:1px solid #333;border-radius:8px;margin:1.1rem 0 0;padding:.1rem .9rem .9rem}
legend{color:#0a84ff;font-size:.75rem;font-weight:600;letter-spacing:.08em;text-transform:uppercase;padding:0 .4rem}
button{width:100%;margin-top:1.2rem;padding:.6rem;border:0;border-radius:6px;background:#0a84ff;color:#fff;font-size:1rem;cursor:pointer}
</style></head><body><div class="card">
<h1>ESP32 World Clock</h1>
)rawliteral";

// One preset-city <select> for a quadrant slot, current selection marked.
static void appendZoneSelect(String &page, const char *label, int slot)
{
    page += "<label>" + String(label) + "<select name=\"zone" + String(slot) + "\">";
    bool matched = false;
    for (int p = 0; p < TZ_PRESET_COUNT; p++)
    {
        bool sel = worldZones[slot].timezone == TZ_PRESETS[p].tz;
        matched |= sel;
        page += "<option value=\"" + String(p) + "\"" + (sel ? " selected" : "") + ">" +
                TZ_PRESETS[p].name + " (" + TZ_PRESETS[p].tz + ")</option>";
    }
    if (!matched)
    {
        // Zone outside the preset list (e.g. hand-edited config): offer to
        // keep it; value -1 is ignored by the POST handler.
        page += "<option value=\"-1\" selected>keep " + worldZones[slot].name + "</option>";
    }
    page += "</select></label>";
}

// On/Off <select> for a boolean setting (home-screen extras, auto-dim).
static void appendToggle(String &page, const char *label, const char *name, bool on)
{
    page += "<label>" + String(label) + "<select name=\"" + name + "\">";
    page += String("<option value=\"1\"") + (on ? " selected" : "") + ">On</option>";
    page += String("<option value=\"0\"") + (!on ? " selected" : "") + ">Off</option>";
    page += "</select></label>";
}

// 0:00 .. 23:00 <option> rows for the night-window hour selects.
static void appendHourOptions(String &page, int selected)
{
    for (int h = 0; h < 24; h++)
    {
        page += "<option value=\"" + String(h) + "\"" +
                (h == selected ? " selected" : "") + ">" + String(h) + ":00</option>";
    }
}

static void handleSettingsPage()
{
    if (!webAuthenticate()) return;

    String page;
    page.reserve(14336);
    page += FPSTR(SETTINGS_PAGE_HEAD);
    page += "<p>Running build: " + String(__DATE__) + " " + __TIME__ +
            " &middot; <a href=\"/update\">Firmware update</a>"
            " &middot; <a href=\"/logs\">Logs</a>"
            " &middot; <a href=\"/wifi-login\">Wi-Fi login</a>"
            " &middot; <a href=\"/api/status\">Status JSON</a>"
            " &middot; <a href=\"/screenshot\">Screenshot</a></p>";
    if (captivePortalActive())
    {
        page += "<p style=\"color:#ff9f0a\">This network needs a browser login "
                "&mdash; the clock is on Wi-Fi but has no internet. "
                "<a href=\"/wifi-login\">Fix it</a>.</p>";
    }
    page += "<form method=\"POST\" action=\"/settings\">";

    // --- Clocks & time ---
    page += "<fieldset><legend>Clocks &amp; time</legend>";
    static const char *slotLabels[4] = {"Top-left clock (home)", "Top-right clock",
                                        "Bottom-left clock", "Bottom-right clock"};
    for (int i = 0; i < 4; i++)
    {
        appendZoneSelect(page, slotLabels[i], i);
    }

    page += "<div class=\"row\"><label>Clock format<select name=\"clk\">";
    page += String("<option value=\"24\"") + (SHOW_24HOUR ? " selected" : "") + ">24 hour</option>";
    page += String("<option value=\"12\"") + (!SHOW_24HOUR ? " selected" : "") + ">12 hour (AM/PM)</option>";
    page += "</select></label>";
    page += "<label>Date format<select name=\"date\">";
    page += String("<option value=\"dmy\"") + (NOT_US_DATE ? " selected" : "") + ">DD/MM/YY</option>";
    page += String("<option value=\"mdy\"") + (!NOT_US_DATE ? " selected" : "") + ">MM/DD/YY</option>";
    page += "</select></label></div>";
    page += "</fieldset>";

    // --- Display ---
    page += "<fieldset><legend>Display</legend>";
    page += "<label>Clock face<select name=\"face\">";
    for (int f = 0; f < FACE_COUNT; f++)
    {
        page += "<option value=\"" + String(f) + "\"" +
                (projectConfig.clockFace == f ? " selected" : "") + ">" +
                clockFaceName(f) + "</option>";
    }
    page += "</select></label>";
    appendToggle(page, "Flip display 180&deg; (upside-down mounting)", "flip",
                 projectConfig.flipDisplay);
    page += "</fieldset>";

    // --- Brightness ---
    page += "<fieldset><legend>Brightness</legend>";
    int pct = map(backlightLevel, 5, 255, 0, 100);
    page += "<label>Brightness (<span id=\"bv\">" + String(pct) + "</span>%)"
            "<input type=\"range\" name=\"bri\" min=\"5\" max=\"255\" value=\"" +
            String(backlightLevel) + "\" oninput=\"document.getElementById('bv')"
            ".textContent=Math.round((this.value-5)*100/250)\"></label>";

    // Auto-dim master switch + night dimming: window (home-zone hours) and
    // the dimmed level (all ignored while the switch is off).
    appendToggle(page, "Auto-dim (light sensor / night window)", "adim",
                 projectConfig.autoBrightness);
    page += "<div class=\"row\"><label>Night dim from<select name=\"nstart\">";
    appendHourOptions(page, projectConfig.nightStartHour);
    page += "</select></label><label>until<select name=\"nend\">";
    appendHourOptions(page, projectConfig.nightEndHour);
    page += "</select></label></div>";
    int npct = map(constrain(projectConfig.nightBrightness, 1, 255), 1, 255, 0, 100);
    page += "<label>Night brightness (<span id=\"nv\">" + String(npct) + "</span>%)"
            "<input type=\"range\" name=\"nbri\" min=\"1\" max=\"255\" value=\"" +
            String(constrain(projectConfig.nightBrightness, 1, 255)) +
            "\" oninput=\"document.getElementById('nv')"
            ".textContent=Math.round((this.value-1)*100/254)\"></label>";
    page += "<p>Night brightness is used when the room is dark (light sensor), "
            "or inside the window above (home-zone time) when the sensor is "
            "unavailable. Equal hours disable the schedule. Auto-dim Off keeps "
            "the backlight at the brightness set above at all times. Saving a "
            "brightness change pauses auto-dim for 2 hours, same as the "
            "on-device controls.</p>";
    page += "</fieldset>";

    // --- World-clock face ---
    // Home-screen extras: each new world-clock face element is individually
    // revertible to the classic look.
    page += "<fieldset><legend>World-clock face</legend>";
    page += "<div class=\"row\">";
    appendToggle(page, "Quadrant grid", "grid", projectConfig.showGrid);
    appendToggle(page, "Sun/moon icons + night colors", "qdn", projectConfig.dayNightIcons);
    page += "</div><div class=\"row\">";
    appendToggle(page, "Home quadrant border", "qhome", projectConfig.homeMarker);
    appendToggle(page, "Weather in quadrants", "qwx", projectConfig.quadWeather);
    page += "</div><div class=\"row\">";
    appendToggle(page, "Daylight bar", "qdb", projectConfig.daylightBar);
    appendToggle(page, "Market-session progress bar", "qmb", projectConfig.marketProgressBar);
    page += "</div><div class=\"row\">";
    appendToggle(page, "Smooth time digits", "qsf", projectConfig.smoothTimeFont);
    appendToggle(page, "Weather alerts on market line", "qwa", projectConfig.weatherAlerts);
    page += "</div>";
    page += "</fieldset>";

    // --- Weather & calendar ---
    page += "<fieldset><legend>Weather &amp; calendar</legend>";
    page += "<div class=\"row\"><label>Temperature unit<select name=\"tunit\">";
    page += String("<option value=\"c\"") + (!projectConfig.useFahrenheit ? " selected" : "") + ">&deg;C</option>";
    page += String("<option value=\"f\"") + (projectConfig.useFahrenheit ? " selected" : "") + ">&deg;F</option>";
    page += "</select></label>";
    page += "<label>Week starts on (calendar)<select name=\"wkstart\">";
    page += String("<option value=\"sun\"") + (!projectConfig.weekStartMonday ? " selected" : "") + ">Sunday</option>";
    page += String("<option value=\"mon\"") + (projectConfig.weekStartMonday ? " selected" : "") + ">Monday</option>";
    page += "</select></label></div>";
    page += "<label>Weather refresh<select name=\"wref\">";
    static const int WREF_CHOICES[] = {5, 10, 15, 20, 30, 60, 120};
    bool wrefListed = false;
    for (int c : WREF_CHOICES)
    {
        bool sel = projectConfig.weatherRefreshMin == c;
        wrefListed |= sel;
        page += "<option value=\"" + String(c) + "\"" + (sel ? " selected" : "") +
                ">every " + String(c) + " min</option>";
    }
    if (!wrefListed) // hand-edited config value outside the preset list
    {
        page += "<option value=\"" + String(projectConfig.weatherRefreshMin) +
                "\" selected>every " + String(projectConfig.weatherRefreshMin) +
                " min</option>";
    }
    page += "</select></label>";
    page += "</fieldset>";

    // --- Network ---
    page += "<fieldset><legend>Network</legend>";
    page += "<label>Hostname (mDNS \"&lt;name&gt;.local\", applied after reboot)"
            "<input type=\"text\" name=\"host\" maxlength=\"32\" value=\"" +
            projectConfig.hostname + "\"></label>";

    // Custom MAC for login-required networks (reboots to apply). See /wifi-login.
    page += "<label>Custom MAC for login networks "
            "(blank = default, applied after reboot &middot; "
            "<a href=\"/wifi-login\">help</a>)"
            "<input type=\"text\" name=\"mac\" maxlength=\"17\" "
            "placeholder=\"AA:BB:CC:DD:EE:FF\" value=\"" +
            projectConfig.staMacOverride + "\"></label>";
    page += "</fieldset>";

    page += "<button type=\"submit\">Save</button></form>";

    // Config backup/restore (/api/config). Restore expects a previously
    // downloaded backup; the device saves it and reboots to apply.
    page += "<p><a href=\"/api/config\" download=\"worldclock-config.json\">Backup config"
            "</a> &middot; restore: <input type=\"file\" id=\"cfg\" accept=\".json\" "
            "style=\"width:auto;color:#ccc\"></p>"
            "<script>document.getElementById('cfg').addEventListener('change',"
            "async function(){if(!this.files[0])return;"
            "var r=await fetch('/api/config',{method:'POST',body:await this.files[0].text()});"
            "alert(await r.text());});</script>";

    // Factory reset (/api/factory-reset). Danger action: wipes settings AND
    // WiFi credentials, then reboots. Confirm before firing.
    page += "<p style=\"margin-top:1.5em\"><button type=\"button\" "
            "style=\"background:#d11;border-color:#d11;color:#fff\" "
            "onclick=\"if(confirm('Erase ALL settings and WiFi credentials and "
            "reboot to a clean, first-boot state?')){"
            "fetch('/api/factory-reset',{method:'POST'})"
            ".then(async r=>alert(await r.text()));}\">"
            "Factory reset</button></p>";

    page += "</div></body></html>";
    webServer.send(200, "text/html", page);
}

static void handleSettingsPost()
{
    if (!webAuthenticate()) return;

    // Timezone changes first - each one persists the config and re-fetches
    // the zone definition (brief blocking network call per changed zone).
    for (int i = 0; i < 4; i++)
    {
        String arg = webServer.arg("zone" + String(i));
        if (arg.length() == 0) continue;
        int idx = arg.toInt();
        if (idx < 0 || idx >= TZ_PRESET_COUNT) continue; // -1 = keep current
        if (worldZones[i].timezone == TZ_PRESETS[idx].tz &&
            worldZones[i].name == TZ_PRESETS[idx].name) continue;
        applyZoneSelection(i, TZ_PRESETS[idx]);
    }

    // Absent fields keep their current value, so a partial POST (scripted
    // curl, say) can't silently flip unrelated settings.
    bool wants24 = webServer.hasArg("clk") ? webServer.arg("clk") != "12" : SHOW_24HOUR;
    bool wantsDmy = webServer.hasArg("date") ? webServer.arg("date") != "mdy" : NOT_US_DATE;
    if (wants24 != SHOW_24HOUR || wantsDmy != NOT_US_DATE)
    {
        SHOW_24HOUR = wants24;
        NOT_US_DATE = wantsDmy;
        saveDisplayPrefs();
    }

    if (webServer.hasArg("face"))
    {
        int face = webServer.arg("face").toInt();
        if (face >= 0 && face < FACE_COUNT && face != projectConfig.clockFace)
        {
            projectConfig.clockFace = face;
            projectConfig.saveConfigFile();
        }
    }

    int bri = webServer.arg("bri").toInt();
    if (bri >= 5 && bri <= 255 && bri != backlightLevel)
    {
        backlightLevel = bri;
        analogWrite(BACKLIGHT_PIN, backlightLevel);
        manualBrightnessUntil = millis() + MANUAL_BRIGHTNESS_HOLD_MS;
        projectConfig.brightness = backlightLevel;
        projectConfig.saveConfigFile();
    }

    // Everything below is gathered into a single config save
    bool cfgDirty = false;
    if (webServer.hasArg("flip"))
    {
        bool v = webServer.arg("flip") == "1";
        if (v != projectConfig.flipDisplay)
        {
            projectConfig.flipDisplay = v;
            // Applies right away: switchToScreen below repaints everything in
            // the new orientation, and touch reads follow the setting.
            tft.setRotation(v ? 3 : 1);
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("tunit"))
    {
        bool f = webServer.arg("tunit") == "f";
        if (f != projectConfig.useFahrenheit)
        {
            projectConfig.useFahrenheit = f;
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("wkstart"))
    {
        bool mon = webServer.arg("wkstart") == "mon";
        if (mon != projectConfig.weekStartMonday)
        {
            projectConfig.weekStartMonday = mon;
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("wref"))
    {
        int v = constrain(webServer.arg("wref").toInt(), 5, 120);
        if (v != projectConfig.weatherRefreshMin)
        {
            projectConfig.weatherRefreshMin = v;
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("adim"))
    {
        bool v = webServer.arg("adim") == "1";
        if (v != projectConfig.autoBrightness)
        {
            projectConfig.autoBrightness = v;
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("nstart"))
    {
        int v = constrain(webServer.arg("nstart").toInt(), 0, 23);
        if (v != projectConfig.nightStartHour)
        {
            projectConfig.nightStartHour = v;
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("nend"))
    {
        int v = constrain(webServer.arg("nend").toInt(), 0, 23);
        if (v != projectConfig.nightEndHour)
        {
            projectConfig.nightEndHour = v;
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("nbri"))
    {
        int v = webServer.arg("nbri").toInt();
        if (v >= 1 && v <= 255 && v != projectConfig.nightBrightness)
        {
            projectConfig.nightBrightness = v;
            cfgDirty = true;
        }
    }
    // World-clock face toggles (grid + home-screen extras)
    struct { const char *arg; bool *value; } extras[] = {
        {"grid", &projectConfig.showGrid},
        {"qsf", &projectConfig.smoothTimeFont},
        {"qdn", &projectConfig.dayNightIcons},
        {"qhome", &projectConfig.homeMarker},
        {"qwx", &projectConfig.quadWeather},
        {"qdb", &projectConfig.daylightBar},
        {"qmb", &projectConfig.marketProgressBar},
        {"qwa", &projectConfig.weatherAlerts},
    };
    for (auto &extra : extras)
    {
        if (!webServer.hasArg(extra.arg)) continue;
        bool v = webServer.arg(extra.arg) == "1";
        if (v != *extra.value)
        {
            *extra.value = v;
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("host"))
    {
        String h = sanitizeHostname(webServer.arg("host"));
        if (h != projectConfig.hostname)
        {
            projectConfig.hostname = h;
            cfgDirty = true;
            Log.println("Hostname changed to \"" + h + "\" - applies after the next reboot");
        }
    }
    // Custom MAC changes need a reboot to take effect (the address is set once,
    // before WiFi connects), so they are handled separately below.
    bool macChanged = false;
    if (webServer.hasArg("mac"))
    {
        String canon = normalizeMac(webServer.arg("mac"));
        if (canon != projectConfig.staMacOverride)
        {
            projectConfig.staMacOverride = canon;
            cfgDirty = true;
            macChanged = true;
        }
    }
    if (cfgDirty)
    {
        projectConfig.saveConfigFile();
    }

    // Repaint the home screen with the new settings, whatever page the
    // on-device UI was showing.
    switchToScreen(SCREEN_HOME);
    Log.println("Settings applied from the web page");

    if (macChanged)
    {
        Log.println("Custom MAC changed to \"" + projectConfig.staMacOverride +
                    "\" - rebooting to apply");
        webServer.sendHeader("Connection", "close");
        webServer.send(200, "text/plain",
                       "Custom MAC saved - rebooting to apply it");
        delay(750); // let the response reach the browser
        ESP.restart();
        return;
    }

    webServer.sendHeader("Location", "/");
    webServer.send(303, "text/plain", "Saved");
}

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
    projectConfig.saveConfigFile();
    Log.println("Config imported via /api/config - rebooting to apply");
    webServer.sendHeader("Connection", "close");
    webServer.send(200, "text/plain", "OK - config saved, rebooting to apply it");
    delay(750); // let the response reach the client
    ESP.restart();
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

// Diagnostics as JSON - the System status page, but scriptable.
static void handleApiStatus()
{
    if (!webAuthenticate()) return;

    DynamicJsonDocument doc(3072);
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
    doc["build"] = String(__DATE__) + " " + __TIME__;
    doc["clockFace"] = clockFaceName(projectConfig.clockFace);
    doc["brightness"] = backlightLevel;
    doc["weatherAgeMin"] = weatherAgeMinutes();

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
    unsigned long nowMs = millis();
    doc["manualBrightnessHoldMin"] =
        (manualBrightnessUntil > nowMs) ? (long)((manualBrightnessUntil - nowMs) / 60000UL) : 0;

    JsonArray zones = doc.createNestedArray("zones");
    for (int i = 0; i < 4; i++)
    {
        JsonObject z = zones.createNestedObject();
        z["name"] = worldZones[i].name;
        z["tz"] = worldZones[i].timezone;
        if (worldZones[i].lastMarketStatus.length() > 0)
        {
            z["market"] = worldZones[i].lastMarketStatus;
        }
    }

    String out;
    serializeJson(doc, out);
    webServer.send(200, "application/json", out);
}

/*-------- Log viewer ----------*/
// The tail of the in-RAM log ring (logBuffer.h) in the browser: /api/logs
// returns it as plain text, /logs is a small auto-refreshing viewer.

static const char LOGS_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>World Clock logs</title>
<style>
body{font-family:ui-monospace,Consolas,monospace;background:#111;color:#ddd;margin:0;padding:1rem}
h1{font-size:1rem;font-family:system-ui,sans-serif;margin:0 .0 .4rem}
a{color:#0a84ff;font-family:system-ui,sans-serif;font-size:.85rem;font-weight:normal}
label{font-family:system-ui,sans-serif;font-size:.8rem;color:#aaa}
pre{white-space:pre-wrap;word-break:break-all;font-size:.78rem;line-height:1.4;margin:.6rem 0 0}
</style></head><body>
<h1>ESP32 World Clock logs &middot; <a href="/">settings</a></h1>
<label><input type="checkbox" id="auto" checked> auto-refresh (2s), timestamps are uptime</label>
<pre id="l">loading...</pre>
<script>
var l=document.getElementById('l'),auto=document.getElementById('auto');
async function load(){
  try{
    var r=await fetch('/api/logs');
    var nearBottom=(window.innerHeight+window.scrollY)>=(document.body.scrollHeight-60);
    l.textContent=await r.text();
    if(nearBottom)window.scrollTo(0,document.body.scrollHeight);
  }catch(e){}
}
load();
setInterval(function(){if(auto.checked)load();},2000);
</script></body></html>
)rawliteral";

static void handleLogsPage()
{
    if (!webAuthenticate()) return;
    webServer.send(200, "text/html", FPSTR(LOGS_PAGE));
}

static void handleApiLogs()
{
    if (!webAuthenticate()) return;
    webServer.send(200, "text/plain", logTail(6144));
}

/*-------- Screenshot (debug) ----------*/
// GET /screenshot streams the panel's current contents as a 24-bit BMP
// (320x240, ~226 KB), read back from the display controller over SPI one
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

    const int w = tft.width();  // 320x240 in both mounting orientations
    const int h = tft.height();
    const uint32_t rowBytes = (uint32_t)w * 3; // 24bpp; 320*3 is 4-aligned
    const uint32_t imgBytes = rowBytes * h;
    const uint32_t fileSize = 54 + imgBytes;

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

    uint16_t line[320];
    uint8_t row[320 * 3];
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
                           "tzlist, status, logs, wifilogin, wififail");
            return;
        }
        Log.println("UI page \"" + name + "\" opened via /api/screen");
    }
    webServer.send(200, "application/json",
                   String("{\"screen\":\"") + uiScreenName() + "\"}");
}

/*-------- Wi-Fi login helper page ----------*/
// Guidance for login-required (captive-portal) networks: shows the clock's
// current MAC and internet status, and the two ways to get online, since portal
// access is granted per MAC. Reachable at /wifi-login and linked from the
// settings page (also the URL logged when a captive portal is detected).

static void handleWifiLoginPage()
{
    if (!webAuthenticate()) return;

    NetReachability net = netReachability();
    const char *statusText = net == NET_ONLINE  ? "Online"
                             : net == NET_CAPTIVE ? "Login required (captive portal)"
                                                  : "No internet response";
    const char *statusColor = net == NET_ONLINE  ? "#30d158"
                              : net == NET_CAPTIVE ? "#ff9f0a"
                                                   : "#ff6961";

    String page;
    page.reserve(4096);
    page += FPSTR(SETTINGS_PAGE_HEAD); // shared dark card + <h1>
    page += "<p><a href=\"/\">&larr; Settings</a></p>";
    page += "<h1 style=\"font-size:1.05rem\">Wi-Fi login help</h1>";
    page += "<p>Internet status: <b style=\"color:" + String(statusColor) +
            "\">" + statusText + "</b></p>";
    page += "<p>Some networks (hotels, offices, campuses, guest Wi-Fi) let a "
            "device join but block the internet until you log in on a web page. "
            "That access is granted per <b>MAC address</b>, and this clock has "
            "no browser to complete the login itself.</p>";
    page += "<p>This device's MAC address:<br><b>" + WiFi.macAddress() + "</b>" +
            (projectConfig.staMacOverride.length() > 0
                 ? " <span style=\"color:#0a84ff\">(cloned)</span>"
                 : "") +
            "</p>";
    page += "<p><b>Option A &mdash; log in through the clock (recommended).</b> "
            "Press the button below (or on-device: Settings &rarr; WiFi login). "
            "The clock opens a temporary hotspot; join it on your phone, complete "
            "the network's login in your browser, and the clock inherits the "
            "access. The on-device screen shows progress.</p>";
    if (wifiRelayActive())
    {
        page += "<p style=\"color:#30d158\">Helper is running &mdash; join "
                "<b>" + wifiRelayApSsid() + "</b> (password " +
                wifiRelayApPassword() + ") on your phone and log in.</p>";
    }
    else
    {
        page += "<form method=\"POST\" action=\"/wifi-login/start\">"
                "<button type=\"submit\">Start login helper</button></form>";
    }
    page += "<p><b>Option B &mdash; register this MAC.</b> If the network has a "
            "device-registration page, add the MAC above so the network "
            "authorizes this clock directly.</p>";
    page += "<p><b>Option C &mdash; clone an authorized device.</b> Log a device "
            "you control (e.g. your phone) into the network, note its Wi-Fi MAC, "
            "and enter it below. The clock then presents that address and "
            "inherits its access. Disable your phone's \"private/random MAC\" "
            "for this network first, and don't keep both on the network at once "
            "with the same MAC.</p>";
    // Minimal form: the settings POST handler only touches fields that are
    // present, so this changes just the MAC (and reboots to apply it).
    page += "<form method=\"POST\" action=\"/settings\">"
            "<label>Cloned MAC (blank = factory MAC)"
            "<input type=\"text\" name=\"mac\" maxlength=\"17\" "
            "placeholder=\"AA:BB:CC:DD:EE:FF\" value=\"" +
            projectConfig.staMacOverride +
            "\"></label>"
            "<button type=\"submit\">Save &amp; reboot</button></form>";
    page += "<p style=\"color:#aaa\">Saving a MAC reboots the clock so the new "
            "address is used from the next connection.</p>";
    page += "</div></body></html>";
    webServer.send(200, "text/html", page);
}

// POST /wifi-login/start: ask the main loop to open the on-device login relay
// helper (it can't switch WiFi modes from this web-server callback safely).
static void handleWifiLoginStart()
{
    if (!webAuthenticate()) return;
    wifiRelayRequest();
    String page;
    page.reserve(1024);
    page += FPSTR(SETTINGS_PAGE_HEAD);
    page += "<p><a href=\"/wifi-login\">&larr; Wi-Fi login</a></p>";
    page += "<h1 style=\"font-size:1.05rem\">Login helper starting</h1>";
    page += "<p>On your phone, join Wi-Fi <b>" + wifiRelayApSsid() +
            "</b> (password " + wifiRelayApPassword() + "), then complete the "
            "network's login in your browser. The clock's screen shows "
            "progress and returns to normal once it is online.</p>";
    page += "</div></body></html>";
    webServer.send(200, "text/html", page);
}

static void setupWebUpdater()
{
    webServer.on("/", HTTP_GET, handleSettingsPage);
    webServer.on("/settings", HTTP_POST, handleSettingsPost);
    webServer.on("/wifi-login", HTTP_GET, handleWifiLoginPage);
    webServer.on("/wifi-login/start", HTTP_POST, handleWifiLoginStart);
    webServer.on("/api/status", HTTP_GET, handleApiStatus);
    webServer.on("/api/config", HTTP_GET, handleApiConfigGet);
    webServer.on("/api/config", HTTP_POST, handleApiConfigPost);
    webServer.on("/api/factory-reset", HTTP_POST, handleApiFactoryReset);
    webServer.on("/logs", HTTP_GET, handleLogsPage);
    webServer.on("/api/logs", HTTP_GET, handleApiLogs);
    webServer.on("/screenshot", HTTP_GET, handleScreenshot);
    webServer.on("/api/screen", handleApiScreen);
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
