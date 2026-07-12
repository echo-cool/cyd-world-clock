# Host unit tests

These are [Unity](https://github.com/ThrowTheSwitch/Unity) tests that run on the
**development machine** (no ESP32 required) via PlatformIO's `native` platform.
They cover the hardware-independent logic that has been factored out of the
firmware into small Arduino-free modules, so the tricky pure logic can be
exercised fast and offline instead of only on-device.

| Test suite | Module under test | What it guards |
|---|---|---|
| `test_wifi_credentials` | `wifiCredentials.cpp` | Boot credential ordering — the "stuck at *System initializing…*" bug: a phone-portal-saved network must be tried, and tried before the compiled-in `secrets.h` pair. |
| `test_datemath` | `dateMath.cpp` | `daysFromCivil` / `civilFromDays` — epoch, day-of-week, leap years, and a full round-trip across 1970–2069. |
| `test_timeformat` | `timeFormat.cpp` | 12/24-hour `HH:MM` rendering, midnight/noon → 12, AM/PM flag, out-of-range safety. |
| `test_textsanitize` | `textSanitize.cpp` | Hostname sanitising (filter/lowercase/trim/cap/default) and MAC parse/normalise. |
| `test_marketsession` | `marketSession.cpp` | Trading-session membership and minutes-to-open/close, including midnight-spanning (overnight) sessions. |
| `test_timerlogic` | `timerLogic.cpp` | Stopwatch/countdown state machines for the timer faces: start/pause/resume/reset, >24h elapsed, exact-zero completion, never-negative remaining, duration clamping, milestone-reminder boundaries (late/multiple crossings, no duplicates, invalid intervals), `HH:MM:SS` formatting, the seconds-hidden `HH:MM` minute display (floor/ceil) and 64-bit long-run timestamps. |

## Running

```sh
pio test -e native
```

The `native` environment compiles only the extracted logic modules (see
`build_src_filter` in `platformio.ini`) plus these tests — never the
Arduino/TFT/WiFi code — and links them with Unity.

### Compiler note (this machine)

PlatformIO's `native` platform needs a host C++ compiler on `PATH`. There is no
system compiler on `PATH` here, but MSYS2's MinGW g++ is installed at
`C:\msys64\mingw64\bin`. Put it on `PATH` for the test run only:

- PowerShell: `./test/run_native_tests.ps1`
- Git Bash:  `PATH="/c/msys64/mingw64/bin:$PATH" pio test -e native`

The firmware build (`pio run -e cyd`) is unaffected — it uses the bundled Xtensa
toolchain and does not need MinGW.

## Adding a test

1. If the logic lives in a hardware-coupled file, extract the pure part into a
   new `*.cpp`/`*.h` with **no** Arduino/TFT/WiFi includes, and have the
   firmware call it (see `wifiCredentials`/`dateMath` for the pattern).
2. Add the new `.cpp` to `build_src_filter` under `[env:native]`.
3. Create `test/test_<name>/test_<name>.cpp` with a `main()` that runs the
   Unity cases (copy an existing suite as a template).
