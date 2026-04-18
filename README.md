# MiniClock

Portable GPS-disciplined UTC clock for ham radio use, built around a Seeeduino XIAO SAMD21, a GPS receiver, and a 1.54 inch Waveshare-compatible e-paper display.

## Features

- Waits for a fresh GPS fix before finishing startup
- Shows UTC time and date on the e-paper display
- Shows Maidenhead grid, county, and park/POTA information when available
- Reads a DS18B20 temperature sensor and displays Fahrenheit
- Shows a compact GPS lock-quality icon in the footer
- Supports minute-by-minute partial e-paper refreshes with periodic full refreshes
- Includes a host-side `timesync.py` utility to set the computer clock from GPS time

## Hardware

Current pin usage in [`miniclock/miniclock.ino`](/Users/gdusbabek/Library/Mobile%20Documents/com~apple~CloudDocs/Personal/codes/miniclock/miniclock/miniclock.ino):

- GPS `TX -> D7`
- GPS `RX -> D6`
- E-paper `CLK -> D8`
- E-paper `DIN/MOSI -> D10`
- E-paper `CS -> D1`
- E-paper `DC -> D5`
- E-paper `RST -> D4`
- E-paper `BUSY -> D3`
- DS18B20 data -> `D2`

Board SPI note:

- `D8 = SCK`
- `D9 = MISO`
- `D10 = MOSI`

## Firmware Behavior

- The sketch initializes serial, display, temperature, and GPS.
- Startup blocks until GPS date and time are fresh; location can settle later.
- Time is displayed as UTC in `HH:MM` format.
- The display refreshes each minute.
- E-paper partial refresh is used between periodic full refreshes.
- The footer includes a small GPS lock-quality icon.
- Temperature below `-50F` is treated as invalid and shown as `??`.

## Serial Commands

The firmware listens on USB serial at `115200`.

- `state`
  Prints latitude, longitude, satellites, Maidenhead, UTC date/time, and whether the location override is active.
- `nmea`
  Toggles raw NMEA sentence logging to USB serial and persists the preference across reboots.
- `location LAT, LON`
  Temporarily overrides the GPS location for 5 minutes.
  Example: `location 29.4241, -98.4936`
- `temp`
  Polls the DS18B20 immediately, prints the formatted temperature, and updates the display.
- `restart`
  Restarts the device.
- `epdtest`
  Runs the e-paper display self-test and redraws the clock face.
- `help`
  Lists the available serial commands.

## NMEA Logging

- NMEA sentence logging defaults to off for new flash settings.
- The `nmea` serial command toggles NMEA passthrough to USB serial on and off.
- The NMEA logging preference is persisted in flash storage across reboots.
- When NMEA logging is enabled, the display shows a small `NMEA` tag in the footer.

## Host Clock Sync

[`timesync.py`](/Users/gdusbabek/Library/Mobile%20Documents/com~apple~CloudDocs/Personal/codes/miniclock/timesync.py) reads GPS time from the device and sets the host system clock.

What it does:

- Finds likely SAMD21 serial devices
- Checks whether the chosen serial port is already owned by another process
- Tries multiple matching serial devices automatically until one works
- Reads NMEA data directly from the serial port
- Parses `RMC` and `ZDA` sentences, including fractional seconds
- Uses microsecond-resolution `settimeofday()` to set the system clock
- Verifies the result and logs success or failure

Run it with `sudo`:

```bash
sudo python3 timesync.py
```

Dry-run mode:

```bash
python3 timesync.py --dry-run
```

If the serial port is busy, the script reports the owning process name and pid and exits.

## Dependencies

Arduino libraries used by the sketch:

- `TinyGPSPlus`
- `GxEPD2`
- `Adafruit_GFX`
- `DallasTemperature`
- `OneWire`
- `Time`
- `Adafruit_SleepyDog`
- `maidenhead`

Python:

- `timesync.py` currently uses only the Python standard library

## Build

Example compile command:

```bash
arduino-cli compile --build-path /tmp/miniclock-build --fqbn Seeeduino:samd:seeed_XIAO_m0 miniclock
```
