#ifndef DRD_GUARD_H
#define DRD_GUARD_H

// Double-reset detector wrapper. drdGuard.cpp is the only translation unit
// that includes <ESP_DoubleResetDetector.h>, so the ESP_DRD_USE_SPIFFS
// storage selection lives in exactly one place instead of having to be
// hand-duplicated (and kept identical) before every include of the library.
//
// Two resets within the detector timeout force the setup portal open on the
// next boot, so every deliberate software reboot must go through
// rebootCleanly() - a plain ESP.restart() shortly after boot would register
// as the second reset of a double-reset.

// Create the detector and report whether this boot is a double reset.
bool drdSetupDetect();

// Poll the detector (clears the double-reset flag once the timeout passes).
// Call from the main loop.
void drdLoop();

// Stop the detector so this restart is not read as a double reset, wait
// delayMs (callers use it as the "let the HTTP response reach the client"
// grace period), then restart. Never returns.
[[noreturn]] void rebootCleanly(unsigned long delayMs = 0);

#endif // DRD_GUARD_H
