#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

// ---------------------------------------------------------------------------
// Internal glue shared by the web-portal translation units:
//   otaUpdate.cpp   - the WebServer instance, ArduinoOTA + /update firmware
//                     upload, webAuthenticate()
//   webSettings.cpp - human-facing HTML pages (settings, logs, wifi-login)
//   webApi.cpp      - machine-facing /api/* endpoints + /screenshot
// Not part of the public surface - other modules keep including otaUpdate.h.
// ---------------------------------------------------------------------------

#include <WebServer.h>

#include "otaUpdate.h" // otaInProgress

// The single HTTP server on port 80, defined in otaUpdate.cpp. Handlers in
// all three files read request state from and reply through this instance.
extern WebServer webServer;

// True if the request may proceed; otherwise a 401 challenge has been sent.
// HTTP Basic auth (username "admin"), active only when OTA_PASSWORD is set
// in secrets.h. Defined in otaUpdate.cpp.
bool webAuthenticate();

#endif // WEB_PORTAL_H
