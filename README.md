# ESP32WorldClock
 
![demo](img/demo.jpg)

# Hardware

- ESP32 with 320 x 240 2.8" LCD display ([ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/))

# Setup

1. [Install Arduino IDE and CH340 USB to UART Driver](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/SETUP.md)
2. Copy `libraries` to `C:\Users\[YOU_USER_NAME]\Documents\Arduino\libraries`
3. Copy `secrets.h.example` to `secrets.h` and fill in your WiFi credentials
   (see "Connect Wifi" below). `secrets.h` is git-ignored so your credentials
   are never committed.
4. Build and upload with Arduino IDE

# Connect Wifi

There are two ways to get the clock online:

1. **Preconfigured credentials (optional):** Set `PRECONFIGURED_SSID` /
   `PRECONFIGURED_PASSWORD` in your local `secrets.h`. On boot the device tries
   these first (5 second timeout). Leave the placeholders unchanged to skip this.
2. **WiFiManager captive portal (fallback):** If the preconfigured connection
   fails — or you double-press reset to force config mode — the device starts a
   captive portal. Connect to SSID `esp32Project` (password `12345678`) and use
   the portal to enter your WiFi, time zone, 24-hour clock and US date format
   preferences. These are saved to flash for next boot.

<p align="center">
  <img src="img/wifi.jpg" alt="wifi" width="45%"/>
  <img src="img/autoConnect.jpg" alt="autoConnect" width="16%"/>
</p>
