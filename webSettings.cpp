// Human-facing web pages served by the device's web server (webPortal.h):
// the settings page ("/") and its POST handler, the live log viewer
// ("/logs"), and the Wi-Fi login helper pages ("/wifi-login"). Split out of
// otaUpdate.cpp - the /update firmware page stays there and the JSON API
// lives in webApi.cpp.

#include "webSettings.h"

#include <WebServer.h>
#include <WiFi.h>

#include "webPortal.h"      // webServer, webAuthenticate - shared web-portal glue

#include "brightness.h"
#include "ClockLogic.h"     // tft, backlightLevel, SHOW_24HOUR, ...
#include "drdGuard.h"       // rebootCleanly - web-triggered reboots
#include "clockFaces.h"     // FACE_COUNT, clockFaceName
#include "deviceIdentity.h" // device label shown in web UI
#include "firmwareInfo.h"   // firmwareGitHash
#include "netCheck.h"       // captive state, MAC parse - /wifi-login + settings
#include "projectConfig.h"  // projectConfig, sanitizeHostname
#include "timerFaces.h"     // countdown state for the settings page + apply
#include "wifiRelay.h"      // login-relay helper trigger + state
#include "uiPages.h"        // TZ_PRESETS, applyZoneSelection

/*-------- Web settings page ----------*/
// Configure the clock from a browser: the same settings as the on-device
// touch UI (timezones, face, formats, brightness). Served at "/"; changes
// are applied on the main loop core (webServer.handleClient runs there), so
// it can safely reuse the touch UI's apply/persist functions.

static const char SETTINGS_PAGE_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>World Clock settings</title>
<style>
:root{color-scheme:dark;--bg:#081421;--panel:#172a42;--panel2:#203a58;--line:#496582;--text:#fff;--muted:#c5d2e1;--blue:#79bdff;--green:#55e5aa;--amber:#ffca70;--red:#ff7b8a;--shadow:0 20px 55px #02071180}
*{box-sizing:border-box}html{scroll-behavior:smooth;scroll-padding-top:18px}body{margin:0;padding:24px;font-family:Inter,ui-sans-serif,system-ui,-apple-system,sans-serif;background:radial-gradient(circle at 12% 0,#12345a 0,transparent 34rem),var(--bg);color:var(--text)}
.app-shell{display:grid;grid-template-columns:240px minmax(0,1fr);width:min(1280px,100%);min-height:calc(100vh - 48px);margin:auto;background:#0f1e30;border:1px solid var(--line);border-radius:24px;box-shadow:var(--shadow)}
.sidebar{position:sticky;top:24px;align-self:start;display:flex;flex-direction:column;gap:22px;height:calc(100vh - 48px);padding:30px 22px;border-right:1px solid var(--line);border-radius:24px 0 0 24px;background:#0c1a2a;backdrop-filter:blur(14px)}.content{min-width:0;padding:clamp(20px,3vw,38px)}
.hero{display:flex;flex-direction:column;align-items:stretch;gap:18px}.eyebrow{color:var(--blue);font-size:.76rem;font-weight:850;letter-spacing:.14em;text-transform:uppercase;margin:0 0 7px}h1{font-size:clamp(1.65rem,4vw,2.5rem);letter-spacing:-.04em;margin:0}h2{font-size:1.15rem;margin:0 0 5px}p{color:var(--muted);font-size:.92rem;line-height:1.65;margin:.4rem 0}a{color:var(--blue);font-weight:600;text-decoration:none}a:hover{text-decoration:underline}
.device-pill{padding:10px 12px;border:1px solid var(--line);border-radius:12px;background:var(--panel);font-size:.82rem;line-height:1.55;color:var(--muted);overflow-wrap:anywhere}.device-pill b{color:var(--green)}
.nav{display:flex;flex-direction:column;gap:5px;min-height:0;overflow:auto}.nav a{flex:none;padding:11px 12px;border-radius:9px;color:#d7e2ee;font-size:.88rem;font-weight:750}.nav a:hover,.nav a:focus-visible{background:var(--panel2);color:#fff;text-decoration:none;outline:2px solid var(--blue);outline-offset:-2px}
.timer-card{display:grid;grid-template-columns:minmax(190px,.8fr) 1.2fr;gap:24px;align-items:center;padding:clamp(18px,3vw,28px);margin-bottom:22px;border:1px solid #4b7ba4;border-radius:20px;background:linear-gradient(135deg,#173a5f,#15283e)}.timer-kicker{display:flex;gap:8px;align-items:center;color:#d6e2ee;font-size:.8rem;font-weight:800;letter-spacing:.1em;text-transform:uppercase}.status-dot{width:8px;height:8px;border-radius:50%;background:var(--muted)}.status-dot.running{background:var(--green);box-shadow:0 0 12px var(--green)}.status-dot.paused{background:var(--amber)}.status-dot.finished{background:var(--red)}.timer-time{font:700 clamp(2.6rem,8vw,5rem)/1 ui-monospace,SFMono-Regular,monospace;letter-spacing:-.07em;margin:12px 0 5px;font-variant-numeric:tabular-nums}.timer-side{display:grid;gap:13px}.duration{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.duration label{text-align:center;margin:0}.duration input{text-align:center;font:700 1.25rem ui-monospace,monospace}.quick{display:flex;gap:7px;flex-wrap:wrap}.quick button{width:auto;margin:0;padding:8px 12px;background:#285078;border:1px solid #5d83a7;font-size:.84rem}.actions{display:flex;gap:9px}.actions button{margin:0}.actions .secondary{background:#34516f}.actions .danger{background:#713247;color:#fff}.timer-msg{min-height:1.2em;color:var(--muted);font-size:.88rem}
.settings-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}.settings-grid>fieldset{margin:0}.settings-grid>.wide{grid-column:1/-1}
label{display:block;color:#e1e9f2;font-size:.88rem;font-weight:700;line-height:1.45;margin:.9rem 0 0}select,input[type=range],input[type=text],input[type=number],input[type=file]{width:100%;margin:.38rem 0 0;padding:.7rem .75rem;background:#0c1a2a;color:var(--text);border:1px solid #5c7895;border-radius:9px;font:inherit;font-size:.95rem}input::placeholder{color:#9fb0c3;opacity:1}input:focus,select:focus{outline:3px solid #79bdff55;border-color:var(--blue)}input[type=range]{padding:.4rem}.row{display:flex;gap:12px}.row label{flex:1}
fieldset{min-width:0;border:1px solid var(--line);border-radius:16px;padding:8px 18px 18px;background:var(--panel)}legend{color:var(--blue);font-size:.79rem;font-weight:850;letter-spacing:.1em;text-transform:uppercase;padding:0 7px}
button{width:100%;margin-top:1.2rem;padding:.78rem 1rem;border:1px solid #72b9ff;border-radius:9px;background:#287fda;color:#fff;font-size:.95rem;font-weight:800;cursor:pointer}button:hover{filter:brightness(1.12)}button:focus-visible{outline:3px solid #fff;outline-offset:2px}button:disabled{opacity:.55;cursor:not-allowed}.savebar{position:sticky;bottom:10px;grid-column:1/-1;display:flex;align-items:center;gap:14px;padding:10px 12px;background:#0c1a2af2;border:1px solid var(--line);border-radius:13px;backdrop-filter:blur(12px)}.savebar button{margin:0}.savebar span{color:var(--muted);font-size:.85rem;white-space:nowrap}
.tools{margin-top:22px;padding-top:20px;border-top:1px solid var(--line)}.tool-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.tool-link{display:block;padding:15px;border:1px solid var(--line);border-radius:12px;background:var(--panel);font-size:.91rem;font-weight:750}.tool-link small{display:block;color:var(--muted);font-size:.82rem;font-weight:500;line-height:1.45;margin-top:5px}.danger-zone{margin-top:16px;padding:16px;border:1px solid #925065;border-radius:14px;background:#351b26}.danger-zone button{background:#cf3851;margin:8px 0 0}
@media(max-width:900px){body{padding:12px}.app-shell{display:block;min-height:calc(100vh - 24px);border-radius:18px;overflow:visible}.sidebar{position:static;height:auto;padding:18px;border-right:0;border-bottom:1px solid var(--line);border-radius:18px 18px 0 0}.hero{flex-direction:row;align-items:center;justify-content:space-between}.device-pill{flex:none;max-width:45%}.nav{flex-direction:row;overflow-x:auto;padding-bottom:2px;scrollbar-width:thin}.nav a{background:var(--panel);white-space:nowrap}.content{padding:20px}.timer-card{grid-template-columns:1fr}.settings-grid{grid-template-columns:1fr}.settings-grid>.wide{grid-column:auto}.tool-grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:560px){body{padding:0;background:var(--bg)}.app-shell{min-height:100vh;border:0;border-radius:0}.sidebar{padding:16px;border-radius:0;gap:15px}.hero{align-items:flex-start;flex-direction:column;gap:12px}.device-pill{max-width:none;width:100%}.content{padding:16px 12px 24px}.row{flex-direction:column;gap:0}.duration{gap:6px}.duration input{font-size:1rem;padding:.6rem .35rem}.actions{flex-direction:column}.quick button{flex:1 1 calc(50% - 7px)}.tool-grid{grid-template-columns:1fr}.savebar{bottom:6px}.savebar span{display:none}.timer-time{font-size:clamp(2.6rem,16vw,4.5rem)}fieldset{padding:7px 13px 15px}}
</style></head><body><div class="app-shell"><aside class="sidebar">
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
    page.reserve(24576);
    page += FPSTR(SETTINGS_PAGE_HEAD);
    page += "<header class=\"hero\"><div><p class=\"eyebrow\">Control center</p>"
            "<h1>World Clock</h1><p>Timers, clocks, display and device settings in one place.</p></div>"
            "<div class=\"device-pill\"><b>&#9679; Online</b> &nbsp;" + deviceLabel() +
            "<br>" + deviceMacAddress() + "</div></header>";
    page += "<nav class=\"nav\" aria-label=\"Settings sections\">"
            "<a href=\"#countdown\">Countdown</a><a href=\"#clocks\">Clocks</a>"
            "<a href=\"#display\">Display</a><a href=\"#services\">Services</a>"
            "<a href=\"#timers\">Timers</a><a href=\"#network\">Network</a>"
            "<a href=\"#maintenance\">Maintenance</a></nav></aside>"
            "<main class=\"content\" id=\"main-content\">";
    if (captivePortalActive())
    {
        page += "<p style=\"color:#ff9f0a\">This network needs a browser login "
                "&mdash; the clock is on Wi-Fi but has no internet. "
                "<a href=\"/wifi-login\">Fix it</a>.</p>";
    }

    uint32_t cdSec = countdownConfiguredSec();
    page += "<section class=\"timer-card\" id=\"countdown\"><div>"
            "<div class=\"timer-kicker\"><span class=\"status-dot\" id=\"cdDot\"></span>"
            "Countdown <span id=\"cdState\">" + String(countdownStateName()) + "</span></div>"
            "<div class=\"timer-time\" id=\"cdTime\">--:--:--</div>"
            "<p>Set a duration and start it here. The clock keeps counting even when another face is shown.</p>"
            "</div><div class=\"timer-side\"><div class=\"duration\">"
            "<label>Hours<input id=\"cdH\" type=\"number\" min=\"0\" max=\"99\" value=\"" + String(cdSec / 3600) + "\"></label>"
            "<label>Minutes<input id=\"cdM\" type=\"number\" min=\"0\" max=\"59\" value=\"" + String((cdSec / 60) % 60) + "\"></label>"
            "<label>Seconds<input id=\"cdS\" type=\"number\" min=\"0\" max=\"59\" value=\"" + String(cdSec % 60) + "\"></label>"
            "</div><div class=\"quick\"><button type=\"button\" data-min=\"5\">5 min</button>"
            "<button type=\"button\" data-min=\"15\">15 min</button>"
            "<button type=\"button\" data-min=\"30\">30 min</button>"
            "<button type=\"button\" data-min=\"60\">1 hour</button></div>"
            "<div class=\"actions\"><button type=\"button\" id=\"cdPrimary\">Start countdown</button>"
            "<button type=\"button\" class=\"danger\" id=\"cdReset\">Reset</button></div>"
            "<div class=\"timer-msg\" id=\"cdMsg\" role=\"status\"></div></div></section>";

    page += "<form class=\"settings-grid\" method=\"POST\" action=\"/settings\">";

    // --- Clocks & time ---
    page += "<fieldset id=\"clocks\"><legend>Clocks &amp; time</legend>";
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
    page += "<fieldset id=\"display\"><legend>Display</legend>";
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
    int pct = brightnessPercent(backlightLevel);
    page += "<label>Brightness (<span id=\"bv\">" + String(pct) + "</span>%)"
            "<input type=\"range\" name=\"bri\" min=\"" + String(BRIGHTNESS_MIN) +
            "\" max=\"" + String(BRIGHTNESS_MAX) + "\" value=\"" +
            String(backlightLevel) + "\" oninput=\"document.getElementById('bv')"
            ".textContent=Math.round((this.value-" + String(BRIGHTNESS_MIN) +
            ")*100/" + String(BRIGHTNESS_MAX - BRIGHTNESS_MIN) + ")\"></label>";

    // Auto-dim master switch + night dimming: window (home-zone hours) and
    // the dimmed level (all ignored while the switch is off).
    appendToggle(page, "Auto-dim (light sensor / night window)", "adim",
                 projectConfig.autoBrightness);
    page += "<div class=\"row\"><label>Night dim from<select name=\"nstart\">";
    appendHourOptions(page, projectConfig.nightStartHour);
    page += "</select></label><label>until<select name=\"nend\">";
    appendHourOptions(page, projectConfig.nightEndHour);
    page += "</select></label></div>";
    int npct = brightnessPercent(projectConfig.nightBrightness);
    page += "<label>Night brightness (<span id=\"nv\">" + String(npct) + "</span>%)"
            "<input type=\"range\" name=\"nbri\" min=\"" + String(BRIGHTNESS_MIN) +
            "\" max=\"" + String(BRIGHTNESS_MAX) + "\" value=\"" +
            String(clampBrightness(projectConfig.nightBrightness)) +
            "\" oninput=\"document.getElementById('nv')"
            ".textContent=Math.round((this.value-" + String(BRIGHTNESS_MIN) +
            ")*100/" + String(BRIGHTNESS_MAX - BRIGHTNESS_MIN) + ")\"></label>";
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
    appendToggle(page, "Weather alerts on weekday line", "qwa", projectConfig.weatherAlerts);
    page += "</div>";
    page += "</fieldset>";

    // --- Weather & calendar ---
    page += "<fieldset id=\"services\"><legend>Weather &amp; calendar</legend>";
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

    // --- Timers ---
    // The stopwatch/countdown clock faces (timerFaces.cpp). The reminder
    // interval drives the milestone banner on both timers; the default
    // duration is what the countdown resets to (on-device +/- adjustments
    // are session-only).
    page += "<fieldset id=\"timers\"><legend>Timer preferences</legend>";
    page += "<div class=\"row\"><label>Reminder interval (1-1440 min)"
            "<input type=\"number\" name=\"tmri\" min=\"1\" max=\"1440\" value=\"" +
            String(projectConfig.timerReminderMin) + "\"></label>";
    page += "<label>Default countdown (1-5999 min)"
            "<input type=\"number\" name=\"cddef\" min=\"1\" max=\"5999\" value=\"" +
            String(projectConfig.countdownDefaultMin) + "\"></label></div>";
    appendToggle(page, "Hide seconds on the timer faces (calm HH:MM display)",
                 "thsec", projectConfig.timerHideSeconds);
    page += "<p>The stopwatch and countdown faces flash a short banner at "
            "every reminder-interval boundary (elapsed / remaining). The "
            "default duration is what the countdown face starts and resets "
            "with; the on-device -30/-5/+5/+30 buttons adjust the current "
            "session only. With seconds hidden the big timer changes just "
            "once a minute (same as the HIDE SEC button on the faces).</p>";
    page += "</fieldset>";

    // --- Network ---
    page += "<fieldset id=\"network\"><legend>Network</legend>";
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

    page += "<div class=\"savebar\"><button type=\"submit\">Save all settings</button>"
            "<span>Changes apply to the clock immediately</span></div></form>";

    // Config backup/restore (/api/config). Restore expects a previously
    // downloaded backup; the device saves it and reboots to apply.
    page += "<section class=\"tools\" id=\"maintenance\"><p class=\"eyebrow\">Maintenance</p>"
            "<h2>Device tools</h2><p>Build " + String(__DATE__) + " " + __TIME__ +
            " &middot; git " + String(firmwareGitHash()) + "</p><div class=\"tool-grid\">"
            "<a class=\"tool-link\" href=\"/update\">Firmware update<small>Install a compiled firmware image</small></a>"
            "<a class=\"tool-link\" href=\"/logs\">Live logs<small>Inspect recent device activity</small></a>"
            "<a class=\"tool-link\" href=\"/wifi-login\">Wi-Fi login<small>Connect through captive portals</small></a>"
            "<a class=\"tool-link\" href=\"/api/status\">Status JSON<small>View diagnostics and timer state</small></a>"
            "<a class=\"tool-link\" href=\"/screenshot\">Screenshot<small>Capture the device display</small></a>"
            "<a class=\"tool-link\" href=\"/api/screen?name=caltouch\">Calibrate touch<small>Open calibration on the clock</small></a>"
            "</div><p><a href=\"/api/config\" download=\"worldclock-config.json\">Download configuration backup"
            "</a></p><label>Restore a configuration backup<input type=\"file\" id=\"cfg\" accept=\".json\"></label>"
            "<script>document.getElementById('cfg').addEventListener('change',"
            "async function(){if(!this.files[0])return;"
            "var r=await fetch('/api/config',{method:'POST',body:await this.files[0].text()});"
            "alert(await r.text());});</script>";

    // Factory reset (/api/factory-reset). Danger action: wipes settings AND
    // WiFi credentials, then reboots. Confirm before firing.
    page += "<div class=\"danger-zone\"><h2>Factory reset</h2>"
            "<p>Erase all settings and Wi-Fi credentials, then return to first-time setup.</p>"
            "<button type=\"button\" "
            "onclick=\"if(confirm('Erase ALL settings and WiFi credentials and "
            "reboot to a clean, first-boot state?')){"
            "fetch('/api/factory-reset',{method:'POST'})"
            ".then(async r=>alert(await r.text()));}\">"
            "Erase device and reset</button></div></section>";

    page += R"rawliteral(<script>
const cd={state:'READY',configuredSec:0,remainingSec:0};
const $=id=>document.getElementById(id);
function hms(sec){sec=Math.max(0,Number(sec)||0);let h=Math.floor(sec/3600),m=Math.floor(sec/60)%60,s=Math.floor(sec)%60;return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0')+':'+String(s).padStart(2,'0')}
function renderCd(){
  $('cdState').textContent=cd.state;$('cdTime').textContent=hms(cd.remainingSec);
  $('cdDot').className='status-dot '+cd.state.toLowerCase();
  let active=cd.state==='RUNNING'||cd.state==='PAUSED';
  ['cdH','cdM','cdS'].forEach(id=>$(id).disabled=active);
  document.querySelectorAll('[data-min]').forEach(b=>b.disabled=active);
  $('cdPrimary').textContent=cd.state==='RUNNING'?'Pause':cd.state==='PAUSED'?'Resume':cd.state==='FINISHED'?'Start again':'Start countdown';
}
async function status(){try{let r=await fetch('/api/countdown',{cache:'no-store'});if(!r.ok)return;let j=await r.json();Object.assign(cd,j.countdown);renderCd()}catch(e){}}
async function command(action){
  let body='action='+encodeURIComponent(action);
  if(action==='start'){let sec=(+$('cdH').value||0)*3600+(+$('cdM').value||0)*60+(+$('cdS').value||0);body+='&durationSec='+sec}
  $('cdMsg').textContent='Sending command...';
  try{let r=await fetch('/api/countdown',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});let j=await r.json();if(!r.ok)throw Error(j.error||'Command failed');Object.assign(cd,j.countdown);$('cdMsg').textContent=j.message||'Done';renderCd()}catch(e){$('cdMsg').textContent=e.message}
}
$('cdPrimary').onclick=()=>command(cd.state==='RUNNING'?'pause':cd.state==='PAUSED'?'resume':'start');
$('cdReset').onclick=()=>command('reset');
document.querySelectorAll('[data-min]').forEach(b=>b.onclick=()=>{$('cdH').value=Math.floor(+b.dataset.min/60);$('cdM').value=+b.dataset.min%60;$('cdS').value=0});
status();setInterval(status,1000);
</script>)rawliteral";

    page += "</main></div></body></html>";
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
    if (bri >= BRIGHTNESS_MIN && bri <= BRIGHTNESS_MAX && bri != backlightLevel)
    {
        backlightLevel = bri;
        setBacklight(backlightLevel);
        markManualBrightness();
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
            tft.setRotation(v ? BOARD_TFT_ROTATION_FLIPPED
                              : BOARD_TFT_ROTATION_NORMAL);
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
    if (webServer.hasArg("tmri"))
    {
        int v = constrain(webServer.arg("tmri").toInt(), 1, 1440);
        if (v != projectConfig.timerReminderMin)
        {
            projectConfig.timerReminderMin = v;
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("cddef"))
    {
        int v = constrain(webServer.arg("cddef").toInt(), 1, 5999);
        if (v != projectConfig.countdownDefaultMin)
        {
            projectConfig.countdownDefaultMin = v;
            // Update the live session too, unless a countdown is running
            timersApplyConfigDefaults();
            cfgDirty = true;
        }
    }
    if (webServer.hasArg("thsec"))
    {
        bool v = webServer.arg("thsec") == "1";
        if (v != projectConfig.timerHideSeconds)
        {
            projectConfig.timerHideSeconds = v;
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
        rebootCleanly(750); // 750 ms lets the response reach the browser
    }

    webServer.sendHeader("Location", "/");
    webServer.send(303, "text/plain", "Saved");
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

/*-------- Route registration ----------*/

// Register the HTML page routes on the shared web server. Called once from
// setupOTA() (otaUpdate.cpp) before webServer.begin().
void webSettingsRegisterRoutes(WebServer &server)
{
    server.on("/", HTTP_GET, handleSettingsPage);
    server.on("/settings", HTTP_POST, handleSettingsPost);
    server.on("/wifi-login", HTTP_GET, handleWifiLoginPage);
    server.on("/wifi-login/start", HTTP_POST, handleWifiLoginStart);
    server.on("/logs", HTTP_GET, handleLogsPage);
}
