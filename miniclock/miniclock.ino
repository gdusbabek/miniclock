#include <stdio.h>
#include <FlashStorage.h>
#include <SPI.h>
#include <Wire.h>
#include <TimeLib.h>
#include <Adafruit_SleepyDog.h>
#include <TinyGPS++.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <maidenhead.h>

#define LOG_GPS_SENTENCES 0

namespace Pins {
// GPS uses the board's Serial1 mapping provided by the SAMD21 core.
constexpr uint8_t GPS_RX = 7;
constexpr uint8_t GPS_TX = 6;

// Suggested e-paper GPIO assignments around hardware SPI.
constexpr uint8_t EPD_CS = 10;
constexpr uint8_t EPD_DC = 5;
constexpr uint8_t EPD_RST = 4;
constexpr uint8_t EPD_BUSY = 3;

// Optional DS18B20 data pin if you add the temperature sensor later.
constexpr uint8_t ONE_WIRE = 2;
}

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t GPS_BAUD = 9600;
constexpr unsigned long SERIAL_WAIT_MS = 3000UL;
constexpr unsigned long DISPLAY_UPDATE_MS = 60UL * 1000UL;
constexpr unsigned long GPS_RESYNC_MS = 60UL * 1000UL;
constexpr unsigned long GPS_DATA_FRESH_MS = 2000UL;
constexpr unsigned long GPS_LOCK_STATUS_MS = 1000UL;
constexpr size_t SERIAL_COMMAND_BUFFER_SIZE = 32;

TinyGPSPlus gps;
OneWire oneWire(Pins::ONE_WIRE);
DallasTemperature tempSensors(&oneWire);

unsigned long lastDisplayUpdateMs = 0;
unsigned long lastGpsResyncMs = 0;
char serialCommandBuffer[SERIAL_COMMAND_BUFFER_SIZE] = {0};
size_t serialCommandLength = 0;

void initializePins() {
  pinMode(Pins::EPD_CS, OUTPUT);
  pinMode(Pins::EPD_DC, OUTPUT);
  pinMode(Pins::EPD_RST, OUTPUT);
  pinMode(Pins::EPD_BUSY, INPUT);

  digitalWrite(Pins::EPD_CS, HIGH);
  digitalWrite(Pins::EPD_DC, LOW);
  digitalWrite(Pins::EPD_RST, HIGH);
}

void initializeSerial() {
  Serial.begin(SERIAL_BAUD);
  const unsigned long serialStartMs = millis();
  while (!Serial && millis() - serialStartMs < SERIAL_WAIT_MS) {
    delay(10);
  }
  Serial1.begin(GPS_BAUD);
}

void initializeDisplay() {
  //SPI.begin();
  // Placeholder until the exact Waveshare driver library is chosen.
}

void showAcquiringLock() {
  Serial.println(F("Acquiring lock..."));
  // Placeholder for first e-paper splash screen.
}

void syncSystemTimeFromGps() {
  if (!gps.time.isValid() || !gps.date.isValid()) {
    return;
  }

  setTime(
    gps.time.hour(),
    gps.time.minute(),
    gps.time.second(),
    gps.date.day(),
    gps.date.month(),
    gps.date.year()
  );
}

bool hasFreshGpsLock() {
  return gps.location.isValid() &&
         gps.date.isValid() &&
         gps.time.isValid() &&
         gps.location.age() <= GPS_DATA_FRESH_MS &&
         gps.date.age() <= GPS_DATA_FRESH_MS &&
         gps.time.age() <= GPS_DATA_FRESH_MS;
}

void consumeGpsData() {
  while (Serial1.available() > 0) {
    const char c = static_cast<char>(Serial1.read());
    gps.encode(c);

#if LOG_GPS_SENTENCES
    Serial.write(c);
#endif
  }
}

void waitForFreshGpsLock() {
  unsigned long lastStatusMs = 0;

  while (!hasFreshGpsLock()) {
    consumeGpsData();

    const unsigned long nowMs = millis();
    if (nowMs - lastStatusMs >= GPS_LOCK_STATUS_MS) {
      Serial.print(F("Waiting for GPS lock"));
      Serial.print(F(" sats="));
      Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
      Serial.print(F(" loc="));
      Serial.print(gps.location.isValid() ? F("Y") : F("N"));
      Serial.print(F(" date="));
      Serial.print(gps.date.isValid() ? F("Y") : F("N"));
      Serial.print(F(" time="));
      Serial.println(gps.time.isValid() ? F("Y") : F("N"));
      lastStatusMs = nowMs;
    }

    delay(10);
  }
  Serial.print(F("GPS is locked with "));
  Serial.print(gps.satellites.value());
  Serial.println(F(" satellites."));
}

void printState() {
  const char* locator = gps.location.isValid()
    ? get_mh(gps.location.lat(), gps.location.lng(), 6)
    : "------";

  Serial.println(F("state"));
  Serial.print(F("latitude: "));
  if (gps.location.isValid()) {
    Serial.println(gps.location.lat(), 6);
  } else {
    Serial.println(F("unavailable"));
  }

  Serial.print(F("longitude: "));
  if (gps.location.isValid()) {
    Serial.println(gps.location.lng(), 6);
  } else {
    Serial.println(F("unavailable"));
  }

  Serial.print(F("satellites: "));
  if (gps.satellites.isValid()) {
    Serial.println(gps.satellites.value());
  } else {
    Serial.println(F("unavailable"));
  }

  Serial.print(F("maidenhead: "));
  Serial.println(locator);

  Serial.print(F("utc date: "));
  if (gps.date.isValid()) {
    Serial.print(gps.date.year());
    Serial.print(F("-"));
    if (gps.date.month() < 10) {
      Serial.print(F("0"));
    }
    Serial.print(gps.date.month());
    Serial.print(F("-"));
    if (gps.date.day() < 10) {
      Serial.print(F("0"));
    }
    Serial.println(gps.date.day());
  } else {
    Serial.println(F("unavailable"));
  }

  Serial.print(F("utc time: "));
  if (gps.time.isValid()) {
    if (gps.time.hour() < 10) {
      Serial.print(F("0"));
    }
    Serial.print(gps.time.hour());
    Serial.print(F(":"));
    if (gps.time.minute() < 10) {
      Serial.print(F("0"));
    }
    Serial.print(gps.time.minute());
    Serial.print(F(":"));
    if (gps.time.second() < 10) {
      Serial.print(F("0"));
    }
    Serial.println(gps.time.second());
  } else {
    Serial.println(F("unavailable"));
  }
}

void handleSerialCommand(const char* command) {
  if (strcmp(command, "state") == 0) {
    printState();
    return;
  }

  if (command[0] != '\0') {
    Serial.print(F("Unknown command: "));
    Serial.println(command);
  }
}

void consumeSerialCommands() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      serialCommandBuffer[serialCommandLength] = '\0';
      handleSerialCommand(serialCommandBuffer);
      serialCommandLength = 0;
      continue;
    }

    if (serialCommandLength < SERIAL_COMMAND_BUFFER_SIZE - 1) {
      serialCommandBuffer[serialCommandLength++] = c;
    }
  }
}

void updateDisplay() {
  if (!gps.location.isValid()) {
    return;
  }

  const char* locator = get_mh(gps.location.lat(), gps.location.lng(), 6);

  Serial.print(F("UTC "));
  Serial.print(year());
  Serial.print(F("-"));
  Serial.print(month());
  Serial.print(F("-"));
  Serial.print(day());
  Serial.print(F(" "));
  Serial.print(hour());
  Serial.print(F(":"));
  Serial.print(minute());
  Serial.print(F("  Grid "));
  Serial.print(locator);
  Serial.print(F("  Sats "));
  Serial.println(gps.satellites.value());
  // Placeholder for full e-paper rendering.
}

void setup() {
  initializePins();
  initializeSerial();
  initializeDisplay();
  // tempSensors.begin();
  showAcquiringLock();
  waitForFreshGpsLock();
  syncSystemTimeFromGps();
  lastGpsResyncMs = millis();
  Serial.println(F("GPS lock acquired."));
}

void loop() {
  consumeGpsData();
  consumeSerialCommands();

  const unsigned long nowMs = millis();

  if (gps.location.isValid() && nowMs - lastGpsResyncMs >= GPS_RESYNC_MS) {
    syncSystemTimeFromGps();
    lastGpsResyncMs = nowMs;
  }

  if (nowMs - lastDisplayUpdateMs >= DISPLAY_UPDATE_MS) {
    updateDisplay();
    lastDisplayUpdateMs = nowMs;
  }
}
