#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

#include <WebServer.h>

// Runtime configuration web page served at http://<device-ip>/
//
// Lets the user edit the four clock zones (label, timezone, market), the home
// zone, the 12/24-hour setting and the date format without re-flashing. On save
// the settings are written to flash and the device reboots to apply them.
//
// Relies on the global `projectConfig` (declared in genericBaseProject.h), so
// this header must be included after that declaration.

WebServer webServer(80);

static const char *MARKET_OPTION_NAMES[] = {"None", "NYSE (New York)", "SSE (Shanghai)", "LSE (London)"};
static const int MARKET_OPTION_COUNT = 4;

String webConfigMarketSelect(int zoneIndex, int selected)
{
  String s = "<select name=\"m" + String(zoneIndex) + "\">";
  for (int m = 0; m < MARKET_OPTION_COUNT; m++)
  {
    s += "<option value=\"" + String(m) + "\"";
    if (m == selected) s += " selected";
    s += ">" + String(MARKET_OPTION_NAMES[m]) + "</option>";
  }
  s += "</select>";
  return s;
}

String webConfigPage()
{
  String html = F("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<title>World Clock Config</title><style>"
                  "body{font-family:sans-serif;max-width:560px;margin:16px auto;padding:0 12px}"
                  "h1{font-size:1.3em}fieldset{margin-bottom:14px}label{display:block;margin:6px 0 2px}"
                  "input[type=text]{width:100%;box-sizing:border-box;padding:6px}"
                  "button{padding:10px 16px;font-size:1em}.row{display:flex;gap:8px}.row>div{flex:1}"
                  "small{color:#666}</style></head><body>");
  html += F("<h1>ESP32 World Clock</h1><form method=\"POST\" action=\"/save\">");

  for (int i = 0; i < PROJECT_NUM_ZONES; i++)
  {
    html += "<fieldset><legend>Zone " + String(i + 1) + "</legend>";
    html += "<label>Label</label><input type=\"text\" name=\"l" + String(i) + "\" value=\"" + projectConfig.zoneLabels[i] + "\" maxlength=\"16\">";
    html += "<label>Timezone (e.g. America/New_York)</label><input type=\"text\" name=\"t" + String(i) + "\" value=\"" + projectConfig.zoneTimezones[i] + "\" maxlength=\"40\">";
    html += "<label>Market</label>" + webConfigMarketSelect(i, projectConfig.zoneMarketId[i]);
    html += "<label><input type=\"radio\" name=\"home\" value=\"" + String(i) + "\"";
    if (projectConfig.homeZoneIndex == i) html += " checked";
    html += "> Use as home zone</label></fieldset>";
  }

  html += "<fieldset><legend>Display</legend>";
  html += String("<label><input type=\"checkbox\" name=\"h24\"") + (projectConfig.twentyFourHour ? " checked" : "") + "> 24-hour clock</label>";
  html += String("<label><input type=\"checkbox\" name=\"usd\"") + (projectConfig.usDateFormat ? " checked" : "") + "> US date format (MM/DD/YY)</label>";
  html += "</fieldset>";

  html += F("<button type=\"submit\">Save &amp; Reboot</button></form>"
            "<p><small>Timezones use the IANA tz database. The device reboots after saving.</small></p>"
            "</body></html>");
  return html;
}

void handleWebRoot()
{
  webServer.send(200, "text/html", webConfigPage());
}

void handleWebSave()
{
  for (int i = 0; i < PROJECT_NUM_ZONES; i++)
  {
    if (webServer.hasArg("l" + String(i))) projectConfig.zoneLabels[i] = webServer.arg("l" + String(i));
    if (webServer.hasArg("t" + String(i))) projectConfig.zoneTimezones[i] = webServer.arg("t" + String(i));
    if (webServer.hasArg("m" + String(i)))
    {
      int m = webServer.arg("m" + String(i)).toInt();
      if (m >= 0 && m < MARKET_OPTION_COUNT) projectConfig.zoneMarketId[i] = m;
    }
  }
  if (webServer.hasArg("home"))
  {
    int h = webServer.arg("home").toInt();
    if (h >= 0 && h < PROJECT_NUM_ZONES) projectConfig.homeZoneIndex = h;
  }
  // Unchecked checkboxes are simply absent from the POST body
  projectConfig.twentyFourHour = webServer.hasArg("h24");
  projectConfig.usDateFormat = webServer.hasArg("usd");

  projectConfig.saveConfigFile();

  webServer.send(200, "text/html",
                 F("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                   "<meta http-equiv=\"refresh\" content=\"8;url=/\"></head><body style=\"font-family:sans-serif\">"
                   "<h2>Saved. Rebooting...</h2><p>This page will return in a few seconds.</p></body></html>"));

  delay(500);
  ESP.restart();
}

void setupWebServer()
{
  webServer.on("/", HTTP_GET, handleWebRoot);
  webServer.on("/save", HTTP_POST, handleWebSave);
  webServer.onNotFound([]() { webServer.send(404, "text/plain", "Not found"); });
  webServer.begin();
  Serial.println("Web config server started on port 80");
}

void handleWebServer()
{
  webServer.handleClient();
}

#endif // WEB_CONFIG_H
