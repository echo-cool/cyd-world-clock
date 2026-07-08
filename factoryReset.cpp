#include "factoryReset.h"

#include <WiFi.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <FS.h>
#include "SPIFFS.h"

#include "logBuffer.h" // Log

void factoryReset()
{
    Log.println("=== FACTORY RESET: erasing all saved settings and credentials ===");

    // 1. WiFi credentials. Erase them through the WiFi API first, while the
    //    driver is still up (this definitely clears the stored AP creds even
    //    if the raw NVS erase below is refused), then take the radio down so
    //    the driver isn't holding NVS handles when the partition is wiped.
    WiFi.disconnect(true, true); // stop STA + erase stored AP config
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(50);

    // 2. Everything else in the NVS partition (ezTime timezone cache and any
    //    other keys). A full partition erase is the cleanest "factory" state.
    esp_err_t nvsErr = nvs_flash_erase();
    if (nvsErr != ESP_OK)
        Log.println(String("  NVS erase warning: ") + esp_err_to_name(nvsErr));

    // 3. Display/format settings, cached calendars and the double-reset flag
    //    all live in SPIFFS - format it for a truly clean slate.
    if (!SPIFFS.format())
        Log.println("  SPIFFS format failed");

    Log.println("Factory reset complete - rebooting to first-boot state");
    delay(500); // let the log lines flush to serial / the log shipper
    ESP.restart();
}
