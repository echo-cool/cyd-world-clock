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

# Touch Screen Controls

The home screen (four world clocks) is split into three touch zones:

| Zone | Action |
| --- | --- |
| Left third | Decrease backlight brightness |
| Center third | Open the **Settings** page |
| Right third | Increase backlight brightness |

## Settings page

- **Change timezones** — tap any of the four clock slots, then pick a city from
  the paged timezone list. Cities with a stock exchange (New York, London,
  Beijing, Tokyo, Hong Kong) automatically show that market's trading status.
  The selection is saved to flash and restored on boot.
- **Clock format** — toggle between 24-hour and 12-hour (AM/PM) display.
- **Date format** — toggle between `DD/MM/YY` and `MM/DD/YY`.
- **Brightness** — `-` / `+` buttons adjust the backlight (also pauses
  auto-brightness for 2 hours, same as the home-screen gesture).
- **System status** — opens a live diagnostics page.

## System status page

Shows WiFi SSID, IP address, signal strength, MAC address, uptime, free heap,
NTP sync count / last sync time and the current UTC time, refreshed every
second. Tap anywhere to go back.
