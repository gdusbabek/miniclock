#!/usr/bin/env python3

from __future__ import annotations

import argparse
import datetime as dt
import glob
import os
import select
import subprocess
import sys
import termios
import time
import ctypes
import ctypes.util


GPS_BAUD = 9600
NMEA_TIMEOUT_SECONDS = 15.0
CONFIDENCE_SAMPLES = 2
CONFIDENCE_WINDOW_SECONDS = 5.0
SYSTEM_TIME_TOLERANCE_SECONDS = 2.0
SET_TIME_OFFSET_SECONDS = 0.0


class Timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_usec", ctypes.c_int)]


LIBC = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
LIBC.settimeofday.argtypes = [ctypes.POINTER(Timeval), ctypes.c_void_p]
LIBC.settimeofday.restype = ctypes.c_int


def log(message: str) -> None:
    timestamp = dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)


def candidate_serial_devices() -> list[str]:
    patterns = [
        "/dev/cu.usbmodem*",
        "/dev/tty.usbmodem*",
        "/dev/cu.usbserial*",
        "/dev/tty.usbserial*",
        "/dev/cu.SLAB_*",
        "/dev/tty.SLAB_*",
    ]
    devices: list[str] = []
    for pattern in patterns:
        devices.extend(glob.glob(pattern))
    return sorted(dict.fromkeys(devices))


def serial_port_owner(path: str) -> tuple[str, int] | None:
    result = subprocess.run(
        ["/usr/sbin/lsof", "-Fpc", path],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode not in (0, 1):
        return None

    command_name: str | None = None
    pid: int | None = None
    for line in result.stdout.splitlines():
        if not line:
            continue
        prefix = line[0]
        value = line[1:]
        if prefix == "p":
            try:
                pid = int(value)
            except ValueError:
                pid = None
        elif prefix == "c":
            command_name = value
            if command_name is not None and pid is not None:
                return command_name, pid
    return None


class SerialReader:
    def __init__(self, path: str, baud: int) -> None:
        self.path = path
        self.fd = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
        self.buffer = bytearray()
        self._configure(baud)

    def _configure(self, baud: int) -> None:
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
        attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
        if hasattr(termios, "CRTSCTS"):
            attrs[2] &= ~termios.CRTSCTS
        attrs[3] = 0
        attrs[4] = termios.B9600 if baud == 9600 else baud
        attrs[5] = termios.B9600 if baud == 9600 else baud
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIFLUSH)

    def readline(self, timeout_seconds: float) -> str | None:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            readable, _, _ = select.select([self.fd], [], [], remaining)
            if not readable:
                continue
            chunk = os.read(self.fd, 4096)
            if not chunk:
                continue
            self.buffer.extend(chunk)
            while b"\n" in self.buffer:
                raw_line, _, remainder = self.buffer.partition(b"\n")
                self.buffer = bytearray(remainder)
                line = raw_line.rstrip(b"\r").decode("ascii", errors="ignore").strip()
                if line:
                    return line
        return None

    def close(self) -> None:
        os.close(self.fd)


def checksum_ok(sentence: str) -> bool:
    if not sentence.startswith("$"):
        return False
    if "*" not in sentence:
        return False
    body, checksum_text = sentence[1:].split("*", 1)
    if len(checksum_text) < 2:
        return False
    checksum = 0
    for char in body:
        checksum ^= ord(char)
    try:
        return checksum == int(checksum_text[:2], 16)
    except ValueError:
        return False


def parse_hms(value: str) -> tuple[int, int, int, int] | None:
    if len(value) < 6:
        return None
    try:
        hour = int(value[0:2])
        minute = int(value[2:4])
        second = int(value[4:6])
        microsecond = 0
        if len(value) > 6 and value[6] == ".":
            fractional = value[7:]
            if fractional:
                microsecond = int((fractional[:6]).ljust(6, "0"))
    except ValueError:
        return None
    return hour, minute, second, microsecond


def parse_rmc(parts: list[str]) -> dt.datetime | None:
    if len(parts) < 10 or parts[2] != "A":
        return None
    hms = parse_hms(parts[1])
    if hms is None or len(parts[9]) != 6:
        return None
    try:
        day = int(parts[9][0:2])
        month = int(parts[9][2:4])
        year = 2000 + int(parts[9][4:6])
        return dt.datetime(year, month, day, hms[0], hms[1], hms[2], hms[3], tzinfo=dt.timezone.utc)
    except ValueError:
        return None


def parse_zda(parts: list[str]) -> dt.datetime | None:
    if len(parts) < 5:
        return None
    hms = parse_hms(parts[1])
    if hms is None:
        return None
    try:
        day = int(parts[2])
        month = int(parts[3])
        year = int(parts[4])
        return dt.datetime(year, month, day, hms[0], hms[1], hms[2], hms[3], tzinfo=dt.timezone.utc)
    except ValueError:
        return None


def extract_utc_datetime(sentence: str) -> dt.datetime | None:
    if not checksum_ok(sentence):
        return None
    parts = sentence.split(",")
    sentence_type = parts[0][-3:]
    if sentence_type == "RMC":
        return parse_rmc(parts)
    if sentence_type == "ZDA":
        return parse_zda(parts)
    return None


def wait_for_confident_time(reader: SerialReader) -> dt.datetime | None:
    nmea_deadline = time.monotonic() + NMEA_TIMEOUT_SECONDS
    first_sentence_seen = False
    recent_samples: list[dt.datetime] = []

    while time.monotonic() < nmea_deadline:
        line = reader.readline(0.5)
        if line is None:
            continue
        if not line.startswith("$"):
            continue
        first_sentence_seen = True
        timestamp = extract_utc_datetime(line)
        if timestamp is None:
            continue

        recent_samples.append(timestamp)
        recent_samples = recent_samples[-CONFIDENCE_SAMPLES:]

        if len(recent_samples) < CONFIDENCE_SAMPLES:
            continue

        if recent_samples[-1] < recent_samples[-2]:
            recent_samples = recent_samples[-1:]
            continue

        if (recent_samples[-1] - recent_samples[0]).total_seconds() > CONFIDENCE_WINDOW_SECONDS:
            recent_samples = recent_samples[-1:]
            continue

        return recent_samples[-1]

    if not first_sentence_seen:
        log("No NMEA sentences were seen within 15 seconds.")
    else:
        log("NMEA sentences were seen, but GPS time never became trustworthy enough.")
    return None


def set_system_clock(target_utc: dt.datetime) -> bool:
    if os.geteuid() != 0:
        log("Insufficient privileges to set the system clock. Please run with sudo.")
        return False

    adjusted_target_utc = target_utc + dt.timedelta(seconds=SET_TIME_OFFSET_SECONDS)
    epoch_seconds = adjusted_target_utc.timestamp()
    whole_seconds = int(epoch_seconds)
    microseconds = int(round((epoch_seconds - whole_seconds) * 1_000_000))
    if microseconds >= 1_000_000:
        whole_seconds += 1
        microseconds -= 1_000_000

    timeval = Timeval(tv_sec=whole_seconds, tv_usec=microseconds)
    if LIBC.settimeofday(ctypes.byref(timeval), None) != 0:
        err = ctypes.get_errno()
        log(f"Failed to set the system clock: {os.strerror(err)}")
        return False

    system_now = dt.datetime.now(dt.timezone.utc)
    delta_seconds = abs((system_now - adjusted_target_utc).total_seconds())
    if delta_seconds > SYSTEM_TIME_TOLERANCE_SECONDS:
        log(
            "System clock was updated, but verification failed: "
            f"expected {adjusted_target_utc.isoformat()}, got {system_now.isoformat()}."
        )
        return False

    log(f"System clock set successfully to {adjusted_target_utc.isoformat()}.")
    return True


def adjusted_target_time(target_utc: dt.datetime) -> dt.datetime:
    return target_utc + dt.timedelta(seconds=SET_TIME_OFFSET_SECONDS)


def main() -> int:
    parser = argparse.ArgumentParser(description="Set the host clock from GPS NMEA time.")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report the time that would be set without changing the system clock.",
    )
    args = parser.parse_args()

    devices = candidate_serial_devices()
    if not devices:
        log("No likely SAMD21 serial device was found.")
        return 1

    for device in devices:
        log(f"Trying serial device {device}.")

        owner = serial_port_owner(device)
        if owner is not None:
            log(
                f"Serial device {device} is already in use by process "
                f"{owner[0]} (pid {owner[1]})."
            )
            continue

        try:
            reader = SerialReader(device, GPS_BAUD)
        except OSError as exc:
            log(f"Failed to open serial device {device}: {exc}")
            continue

        try:
            target_utc = wait_for_confident_time(reader)
            if target_utc is None:
                log(f"Did not get usable GPS time from {device}.")
                continue

            adjusted_utc = adjusted_target_time(target_utc)
            log(f"GPS time established as {target_utc.isoformat()}.")

            if args.dry_run:
                log(f"Dry run: would set system clock to {adjusted_utc.isoformat()}.")
                return 0

            return 0 if set_system_clock(target_utc) else 1
        finally:
            reader.close()

    log("Unable to find a usable GPS serial device.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
