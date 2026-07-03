# ESP32WorldClock
 
![demo](img/demo.jpg)

# Hardware

- ESP32 with 320 x 240 2.8" LCD display ([ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/))

# Setup

1. Copy `secrets.h.example` to `secrets.h` and fill in your WiFi credentials
   (see "Connect Wifi" below). `secrets.h` is git-ignored so your credentials
   are never committed.
2. Build and upload with one of the toolchains below. All of them use the
   vendored `libraries/` folder in this repo (its TFT_eSPI copy carries the
   display config for the CYD), so no library installation is needed except
   for the Arduino IDE.

## Building

**PlatformIO** (fastest incremental builds):

```
pio run                 # build
pio run -t upload       # build + flash
pio device monitor      # serial monitor (115200 baud)
```

**arduino-cli**:

```
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --libraries libraries .
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs .
```

**Arduino IDE**:

1. [Install Arduino IDE and CH340 USB to UART Driver](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/SETUP.md)
2. Copy `libraries` to `C:\Users\[YOU_USER_NAME]\Documents\Arduino\libraries`
3. Select board "ESP32 Dev Module" and set Tools → Partition Scheme →
   "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)", then build and upload

## Over-the-air updates

Once a build with OTA support is on the device, later updates can go over
WiFi — no USB cable needed. The device advertises itself as `esp32worldclock`
on mDNS (its IP is also shown on the System status page), and shows a
progress bar on the display during the transfer before rebooting into the
new firmware. There are two ways in:

- **Web page** (no tools needed): browse to `http://esp32worldclock.local/update`
  (or follow the *Firmware update* link from the settings page at
  `http://<device-ip>/`), pick a compiled firmware image and press
  *Update firmware*. The `.bin` to upload is
  `.pio/build/cyd/firmware.bin` for PlatformIO, the exported binary from
  Arduino IDE → Sketch → Export Compiled Binary, or add
  `--output-dir build` to the `arduino-cli compile` command. The page shows
  upload progress and the running build's compile timestamp.
- **PlatformIO / espota**: uncomment the `espota` lines in `platformio.ini`
  and `pio run -t upload` as usual. In the Arduino IDE, pick the network
  port named `esp32worldclock` under Tools → Port.

OTA is unauthenticated by default (anyone on your WiFi can flash the
device); set `OTA_PASSWORD` in `secrets.h` to protect both paths — the web
page then asks for HTTP Basic credentials (username `admin`), and espota
needs `upload_flags = --auth=<password>`.

Display settings (timezones, formats, brightness, face) and WiFi credentials
survive OTA updates — only the app partition is rewritten.

## Partition scheme

All three toolchains should use the **Minimal SPIFFS** partition scheme (two
1.9MB OTA app slots + 128KB SPIFFS). PlatformIO picks it up automatically from
`platformio.ini`; for arduino-cli / Arduino IDE it is selected as shown above.
The firmware fills ~96% of the default scheme's 1.31MB app slots, so this
scheme is what leaves room for the binary to grow (the sketch keeps only a
<1KB config JSON in SPIFFS, so the smaller filesystem costs nothing).

Note: the first flash after switching partition schemes relocates the SPIFFS
region, so the on-device display settings (timezones, clock/date format,
brightness) reset to defaults once. WiFi credentials and the timezone cache
live in NVS and survive.

# Connect Wifi

There are two ways to get the clock online:

1. **Preconfigured credentials (optional):** Set `PRECONFIGURED_SSID` /
   `PRECONFIGURED_PASSWORD` in your local `secrets.h`. On boot the device tries
   these first (up to 10 attempts, 5 seconds each). Leave the placeholders
   unchanged to skip this.
2. **WiFiManager captive portal (fallback):** If the preconfigured connection
   fails — or you double-press reset to force config mode — the device starts a
   captive portal. Connect to SSID `esp32Project` (password `12345678`) and use
   the portal to enter your WiFi, time zone, 24-hour clock and US date format
   preferences. These are saved to flash for next boot.

If nobody uses the portal within 5 minutes, the device reboots and retries the
whole sequence (preconfigured credentials first). So after a power cut where
the router comes back later than the clock, the clock reconnects on its own —
no button pressing needed.

<p align="center">
  <img src="img/wifi.jpg" alt="wifi" width="45%"/>
  <img src="img/autoConnect.jpg" alt="autoConnect" width="16%"/>
</p>

# Touch Screen Controls

The home screen is split into three touch zones (the same on every clock
face):

| Zone | Action |
| --- | --- |
| Left third | Decrease backlight brightness |
| Center third | Open the **Settings** page |
| Right third | Increase backlight brightness |

# Clock faces

The home screen has five faces, cycled with the **Clock face** button on the
settings page (the choice is saved to flash):

- **World clock** — the classic four-quadrant view: one timezone per quadrant
  with date, day-offset vs. home and stock market status. On a public
  holiday in a zone's country, that quadrant's day line turns gold and shows
  the holiday's name next to the day (e.g. `WED - INDEPENDENCE DAY`); the
  date stays visible as usual.
- **Big clock** — the home zone (top-left quadrant) in 75px digits with date
  and market status, plus a mini strip of the other three zones' times along
  the bottom.
- **Calendar** — a month calendar for the home zone with today highlighted,
  and the current time in the header. Public holidays are marked in gold
  (today's highlight box also turns gold on a holiday), and a footer line
  names today's holiday — or the next upcoming one (`NEXT: 25 DEC -
  CHRISTMAS DAY`).
- **Weather** — current temperature and conditions for all four configured
  cities (with each city's local time), from the free
  [Open-Meteo](https://open-meteo.com) API — no API key needed. A background
  task fetches every 20 minutes regardless of which face is showing, so the
  data is ready the moment the face opens and the clock never pauses. Weather
  is only available for cities picked from the preset timezone list, since it
  needs their coordinates.
- **Markets** — every exchange the clock knows about (NYSE, LSE, SSE, TSE,
  HKEX) at a glance, independent of which cities occupy the quadrants: one
  row per exchange with its local time and the same colored
  open/closed/countdown status as the quadrant view. These rows tick on
  built-in timezone rules, so the face works even without the timezone
  server.

## Settings page

- **Change timezones** — tap any of the four clock slots, then pick a city from
  the paged timezone list. Cities with a stock exchange (New York, London,
  Beijing, Tokyo, Hong Kong) automatically show that market's trading status;
  while an exchange is closed the line counts down to its next regular open
  (e.g. `NYSE OPENS IN 5H 03M`). Full-day exchange holidays are respected —
  the status shows closed on holidays and the countdown skips them; half-day
  early closes are not modeled. The selection is saved to flash and restored
  on boot.

  Exchange holiday calendars keep themselves current: once a week the device fetches
  [`marketHolidays.json`](marketHolidays.json) from this repository over
  HTTPS and caches it in flash, so updating that file (when an exchange
  publishes next year's schedule) reaches every clock within a week — no
  reflash needed. Compiled-in tables (2026–2027 for NYSE/LSE, 2026 for
  SSE/TSE/HKEX, in `marketHolidays.cpp`) serve as the offline fallback. Type
  `HOLIDAYS` in the serial monitor to inspect the active calendars or force a
  refetch, and set `MARKET_HOLIDAYS_URL` in `secrets.h` to point a forked
  device at your own copy of the file.
- **Clock face** — cycle between the four home-screen faces (see above).
- **Clock format** — toggle between 24-hour and 12-hour (AM/PM) display.
- **Date format** — toggle between `DD/MM/YY` and `MM/DD/YY`.
- **Brightness** — `-` / `+` buttons adjust the backlight (also pauses
  auto-brightness for 2 hours, same as the home-screen gesture). The level is
  saved and restored on the next boot, and is used as the daytime target by
  auto-brightness.
- **System status** — opens a live diagnostics page.

## Public holidays

The world-clock quadrants and the calendar face mark each zone's public
holidays by name (see the face descriptions above). The names come from the
free [Nager.Date](https://date.nager.at) API — no key needed — fetched in the
background per zone country (one small request per country-year: on boot,
weekly, when a zone changes and at the year rollover), so the clock itself
never pauses. Only nationwide holidays are shown; regional ones are filtered
out. Dubai and Mumbai have no calendars on that API, so those zones simply
show no holidays. Type `HOLIDAYS` in the serial monitor to see what data
each zone currently has.

## Web settings page

Everything on the settings page can also be changed from a browser: go to
`http://esp32worldclock.local/` (or the device IP shown on the System status
page) to pick the four timezones, clock face, clock/date format and
brightness without touching the device. The page also links to the firmware
updater (`/update`) and a scriptable diagnostics endpoint
(`/api/status`, JSON: IP, RSSI, uptime, heap, NTP syncs, zones, market
status...). If `OTA_PASSWORD` is set in `secrets.h`, the same HTTP Basic
credentials (username `admin`) protect these pages.

## Auto-brightness

The clock dims itself using the CYD's onboard light sensor (LDR on GPIO 34):
when the room goes dark the backlight fades to minimum, and it fades back to
the saved brightness when the lights come on. The LDR circuit is unreliable
on some CYD board revisions, so the sensor is only trusted after its reading
has actually been seen to move; until then the clock falls back to a fixed
schedule (dim between 1–7 AM home-zone time). Type `LDR` in the serial
monitor to see the live readings, and set `LDR_DARK_IS_HIGH` to 0 in
`ClockLogic.h` if your board's sensor reads inverted. Manual brightness
changes (touch gesture or settings page) always win for 2 hours.

## System status page

Shows WiFi SSID, IP address, signal strength, MAC address, uptime, free heap,
NTP sync count / last sync time and the current UTC time, refreshed every
second. Tap anywhere to go back.

# Timekeeping

- Time is synced over NTP every 30 minutes (ezTime).
- Timezone definitions are cached in flash after the first successful lookup,
  so later boots get correct local times even when the timezone server
  (`timezoned.rop.nl`) or the network is unreachable. Cached entries refresh
  automatically once they are older than 6 months, and immediately whenever a
  quadrant's timezone is changed from the settings page.
- Every preset city also carries built-in POSIX timezone rules (including
  DST transitions) as a last resort: if the timezone server is down *and*
  nothing usable is cached — e.g. a first boot, or changing a zone while the
  server is unreachable — the zone still shows correct local time instead of
  falling back to UTC.
