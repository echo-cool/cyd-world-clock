#ifndef WEB_API_H
#define WEB_API_H

// ---------------------------------------------------------------------------
// Machine-facing HTTP endpoints (the JSON API): /api/status, /api/config
// backup/restore, /api/factory-reset, /api/countdown, /api/logs, and the
// /screenshot + /api/screen + /api/touch debug endpoints. Split out of
// otaUpdate.cpp, which keeps the /update firmware page and the shared
// WebServer instance (see webPortal.h).
// ---------------------------------------------------------------------------

#include <WebServer.h>

// Register the JSON API + debug endpoint routes on the shared web server.
// Called once from setupOTA() (otaUpdate.cpp) before webServer.begin().
void webApiRegisterRoutes(WebServer &server);

#endif // WEB_API_H
