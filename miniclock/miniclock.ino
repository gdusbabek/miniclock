#include <stdio.h>
#include <string.h>
#include <FlashStorage.h>
#include <SPI.h>
#include <Wire.h>
#include <TimeLib.h>
#include <Adafruit_SleepyDog.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <TinyGPS++.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <maidenhead.h>

#define LOG_GPS_SENTENCES 0
#define GxEPD2_DISPLAY_CLASS GxEPD2_BW
#define GxEPD2_DRIVER_CLASS GxEPD2_154_D67
#define MAX_DISPLAY_BUFFER_SIZE 15000ul
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) ? EPD::HEIGHT : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))

namespace Pins {
// GPS uses the board's Serial1 mapping provided by the SAMD21 core.
constexpr uint8_t GPS_RX = 7;
constexpr uint8_t GPS_TX = 6;

// Hardware SPI is fixed on this board: CLK/SCK=D8, MISO=D9, DIN/MOSI=D10.
// Keep the control pins off those dedicated SPI pins.
constexpr uint8_t EPD_CS = 1;
constexpr uint8_t EPD_DC = 5;
constexpr uint8_t EPD_RST = 4;
constexpr uint8_t EPD_BUSY = 3;

// Optional DS18B20 data pin if you add the temperature sensor later.
constexpr uint8_t ONE_WIRE = 2;
}

constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t GPS_BAUD = 9600;
constexpr unsigned long SERIAL_WAIT_MS = 3000UL;
constexpr unsigned long ACQUIRING_GPS_MIN_MS = 5000UL;
constexpr unsigned long DISPLAY_UPDATE_MS = 60UL * 1000UL;
constexpr unsigned long GPS_RESYNC_MS = 60UL * 1000UL;
constexpr unsigned long GPS_DATA_FRESH_MS = 2000UL;
constexpr unsigned long GPS_LOCK_STATUS_MS = 1000UL;
constexpr size_t SERIAL_COMMAND_BUFFER_SIZE = 32;

GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)> display(
  GxEPD2_DRIVER_CLASS(Pins::EPD_CS, Pins::EPD_DC, Pins::EPD_RST, Pins::EPD_BUSY)
);

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
  SPI.begin();
  display.init(115200, true, 2, false);
  display.setRotation(0);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();
}

bool textFits(const GFXfont* font, const char* text, int16_t maxWidth, int16_t maxHeight) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  display.setFont(font);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return w <= maxWidth && h <= maxHeight;
}

void drawCenteredText(const char* text, const GFXfont* font, int16_t centerX, int16_t centerY) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  display.setFont(font);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  const int16_t x = centerX - ((int16_t)w / 2) - x1;
  const int16_t y = centerY - ((int16_t)h / 2) - y1;
  display.setCursor(x, y);
  display.print(text);
}

void renderDisplayLines(const char* line1,
                        const char* line2 = "",
                        const char* line3 = "",
                        const char* line4 = "",
                        const char* line5 = "") {
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(0, 16);
    display.println(line1);
    if (line2[0] != '\0') {
      display.setCursor(0, 36);
      display.println(line2);
    }
    if (line3[0] != '\0') {
      display.setCursor(0, 56);
      display.println(line3);
    }
    if (line4[0] != '\0') {
      display.setCursor(0, 76);
      display.println(line4);
    }
    if (line5[0] != '\0') {
      display.setCursor(0, 96);
      display.println(line5);
    }
  } while (display.nextPage());
  display.hibernate();
}

void showAcquiringGps() {
  Serial.println(F("Acquiring GPS..."));
  const GFXfont* font = textFits(&FreeSansBold24pt7b, "Acquiring GPS", display.width() - 8, display.height() - 8)
    ? &FreeSansBold24pt7b
    : &FreeSansBold18pt7b;

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawCenteredText("Acquiring GPS", font, display.width() / 2, display.height() / 2);
  } while (display.nextPage());
  display.hibernate();
}

void holdAcquiringGpsScreen() {
  const unsigned long startMs = millis();
  while (millis() - startMs < ACQUIRING_GPS_MIN_MS) {
    consumeGpsData();
    delay(10);
  }
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
  const char* locator = gps.location.isValid()
    ? get_mh(gps.location.lat(), gps.location.lng(), 6)
    : "------";

  char timeLine[16] = {0};
  char line1[32] = {0};
  char line2[32] = {0};
  char line3[32] = {0};
  char line4[32] = {0};

  snprintf(timeLine, sizeof(timeLine), "%02d:%02d:%02d", hour(), minute(), second());
  snprintf(line1, sizeof(line1), "UTC %04d-%02d-%02d", year(), month(), day());
  snprintf(line2, sizeof(line2), "Grid %s", locator);
  snprintf(line3, sizeof(line3), "Sats %lu", gps.satellites.isValid() ? gps.satellites.value() : 0UL);

  if (gps.location.isValid()) {
    snprintf(line4, sizeof(line4), "%.2f %.2f", gps.location.lat(), gps.location.lng());
  } else {
    snprintf(line4, sizeof(line4), "No location");
  }

  const GFXfont* timeFont = &FreeMonoBold24pt7b;
  if (!textFits(timeFont, timeLine, display.width() - 8, 40)) {
    timeFont = &FreeMonoBold18pt7b;
  }
  if (!textFits(timeFont, timeLine, display.width() - 8, 40)) {
    timeFont = &FreeMonoBold12pt7b;
  }

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawCenteredText(timeLine, timeFont, display.width() / 2, 28);
    display.setFont();
    display.setCursor(0, 68);
    display.println(line1);
    display.setCursor(0, 88);
    display.println(line2);
    display.setCursor(0, 108);
    display.println(line3);
    display.setCursor(0, 128);
    display.println(line4);
  } while (display.nextPage());
  display.hibernate();

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
}

void setup() {
  initializePins();
  initializeSerial();
  initializeDisplay();
  showAcquiringGps();
  holdAcquiringGpsScreen();
  // tempSensors.begin();
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
