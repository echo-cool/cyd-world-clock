#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <Arduino.h>

// Stable human/device identity used in logs, setup hotspot SSIDs, and startup
// screens. The label is "<hostname>-<last six MAC hex>", matching the log
// shipper device field.
String deviceMacAddress();
String deviceMacSuffix();
String deviceLabel();
String setupPortalSsid();

#endif // DEVICE_IDENTITY_H
