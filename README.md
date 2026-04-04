# MiniClock

Portable GPS-disciplined UTC clock for ham radio use, built around a Seeeduino XIAO SAMD21, a GPS receiver, and a 1.54 inch Waveshare-compatible e-paper display.

## Features

- Waits for a fresh GPS fix before finishing startup
- Shows UTC time and date on the e-paper display
- Shows Maidenhead grid, county, and park/POTA information when available
- Reads a DS18B20 temperature sensor and displays Fahrenheit
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
- Momentary button to toggle NMEA logging -> `D0` to `GND`

Board SPI note:

- `D8 = SCK`
- `D9 = MISO`
- `D10 = MOSI`

## Firmware Behavior

- The sketch initializes serial, display, temperature, and GPS.
- Startup blocks until GPS location, date, and time are all fresh.
- Time is displayed as UTC in `HH:MM` format.
- The display refreshes each minute.
- E-paper partial refresh is used between periodic full refreshes.
- Temperature below `-50F` is treated as invalid and shown as `??`.

## Serial Commands

The firmware listens on USB serial at `115200`.

- `state`
  Prints latitude, longitude, satellites, Maidenhead, UTC date/time, and whether the location override is active.
- `location LAT, LON`
  Temporarily overrides the GPS location for 5 minutes.
  Example: `location 29.4241, -98.4936`
- `temp`
  Polls the DS18B20 immediately, prints the formatted temperature, and updates the display.
- `restart`
  Restarts the device.

## NMEA Logging Button

- NMEA sentence logging defaults to off at startup.
- Pressing the `D0` button toggles NMEA passthrough to USB serial on and off.
- The button handler is debounced with `Bounce2`.
- Button events are ignored until startup is complete, which avoids false triggers during boot.

## Host Clock Sync

[`timesync.py`](/Users/gdusbabek/Library/Mobile%20Documents/com~apple~CloudDocs/Personal/codes/miniclock/timesync.py) reads GPS time from the device and sets the host system clock.

What it does:

- Finds likely SAMD21 serial devices
- Checks whether the chosen serial port is already owned by another process
- Reads NMEA data directly from the serial port
- Parses `RMC` and `ZDA` sentences, including fractional seconds
- Uses microsecond-resolution `settimeofday()` to set the system clock
- Verifies the result and logs success or failure

Run it with `sudo`:

```bash
sudo python3 timesync.py
```

If the serial port is busy, the script reports the owning process name and pid and exits.

## Dependencies

Arduino libraries used by the sketch:

- `TinyGPSPlus`
- `GxEPD2`
- `Adafruit_GFX`
- `Bounce2`
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
