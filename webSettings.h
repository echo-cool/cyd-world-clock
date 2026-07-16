#ifndef WEB_SETTINGS_H
#define WEB_SETTINGS_H

// ---------------------------------------------------------------------------
// Human-facing web pages: the settings page ("/") and its POST handler
// ("/settings"), the live log viewer ("/logs") and the Wi-Fi login helper
// pages ("/wifi-login"). Split out of otaUpdate.cpp, which keeps the /update
// firmware page and the shared WebServer instance (see webPortal.h).
// ---------------------------------------------------------------------------

#include <WebServer.h>

// Register the HTML page routes on the shared web server. Called once from
// setupOTA() (otaUpdate.cpp) before webServer.begin().
void webSettingsRegisterRoutes(WebServer &server);

#endif // WEB_SETTINGS_H
