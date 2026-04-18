#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <FlashStorage.h>
#include <SPI.h>
#include <Wire.h>
#include <TimeLib.h>
#include <Adafruit_SleepyDog.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <TinyGPS++.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <maidenhead.h>

#define GxEPD2_DISPLAY_CLASS GxEPD2_BW
#define GxEPD2_DRIVER_CLASS GxEPD2_154_D67
#define MAX_DISPLAY_BUFFER_SIZE 15000ul
#define MAX_HEIGHT(EPD) (EPD::HEIGHT <= MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8) ? EPD::HEIGHT : MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 8))

namespace Pins {
// GPS uses the board's Serial1 mapping provided by the SAMD21 core.
constexpr uint8_t GPS_RX = 7;
constexpr uint8_t GPS_TX = 6;

// Hardware SPI is fixed on this board: CLK/SCK=D8, MISO=D9, DIN/MOSI=D10.
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
constexpr unsigned long LOCATION_OVERRIDE_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long GPS_RESYNC_MS = 60UL * 1000UL;
constexpr unsigned long GPS_DATA_FRESH_MS = 2000UL;
constexpr unsigned long GPS_LOCK_STATUS_MS = 1000UL;
constexpr size_t SERIAL_COMMAND_BUFFER_SIZE = 64;
constexpr uint8_t FULL_REFRESH_INTERVAL_MINUTES = 6;
constexpr uint16_t PARTIAL_HEADER_HEIGHT = 100;
constexpr uint16_t PARTIAL_FOOTER_Y = 168;
constexpr uint16_t PARTIAL_FOOTER_HEIGHT = 32;
constexpr float MIN_REASONABLE_TEMP_F = -50.0f;
constexpr uint32_t SETTINGS_MAGIC = 0x4D434C4BUL;

GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)> display(
  GxEPD2_DRIVER_CLASS(Pins::EPD_CS, Pins::EPD_DC, Pins::EPD_RST, Pins::EPD_BUSY)
);

TinyGPSPlus gps;
OneWire oneWire(Pins::ONE_WIRE);
DallasTemperature tempSensors(&oneWire);

struct StoredSettings {
  uint32_t magic;
  uint8_t logGpsSentences;
};

FlashStorage(settingsStorage, StoredSettings);

unsigned long lastGpsResyncMs = 0;
int lastDisplayedYear = -1;
int lastDisplayedDay = -1;
int lastDisplayedHour = -1;
int lastDisplayedMinute = -1;
bool lastDisplayedUsedLocationOverride = false;
char lastDisplayedLocator[7] = "";
char serialCommandBuffer[SERIAL_COMMAND_BUFFER_SIZE] = {0};
size_t serialCommandLength = 0;
bool locationOverrideActive = false;
double locationOverrideLat = 0.0;
double locationOverrideLon = 0.0;
unsigned long locationOverrideExpiresMs = 0;
uint8_t minuteUpdatesSinceFullRefresh = 0;
bool displayNeedsFullRefresh = false;
float cachedTemperatureF = -1000.0f;
bool logGpsSentences = false;

void updateDisplay(bool forceFullRefresh = false);

StoredSettings loadSettings() {
  return settingsStorage.read();
}

void saveSettings() {
  StoredSettings settings = {SETTINGS_MAGIC, static_cast<uint8_t>(logGpsSentences ? 1 : 0)};
  settingsStorage.write(settings);
}

void loadSettingsIntoRuntime() {
  const StoredSettings settings = loadSettings();
  if (settings.magic != SETTINGS_MAGIC) {
    logGpsSentences = false;
    return;
  }
  logGpsSentences = settings.logGpsSentences != 0;
}

const char* const MONTH_NAMES[] = {
  "January",
  "February",
  "March",
  "April",
  "May",
  "June",
  "July",
  "August",
  "September",
  "October",
  "November",
  "December"
};

const char* countyForGrid(const char* locator) {
  char normalized[7] = {0};
  for (size_t i = 0; i < 6 && locator[i] != '\0'; ++i) {
    normalized[i] = static_cast<char>(toupper(static_cast<unsigned char>(locator[i])));
  }

  if (strcmp(normalized, "EL09UO") == 0) return "Guadalupe Co";
  if (strcmp(normalized, "EL19EO") == 0) return "Gonzales Co";
  if (strcmp(normalized, "EL19FO") == 0) return "Gonzales Co";
  if (strcmp(normalized, "EL09RU") == 0) return "Comal Co";
  if (strcmp(normalized, "EL09SU") == 0) return "Comal Co";
  if (strcmp(normalized, "EL19DU") == 0) return "Caldwell Co";
  if (strcmp(normalized, "EL09SH") == 0) return "Bexar Co";
  if (strcmp(normalized, "EL09SI") == 0) return "Bexar Co";
  return "";
}

const char* parkForGrid(const char* locator) {
  char normalized[7] = {0};
  for (size_t i = 0; i < 6 && locator[i] != '\0'; ++i) {
    normalized[i] = static_cast<char>(toupper(static_cast<unsigned char>(locator[i])));
  }

  if (strcmp(normalized, "EL09UO") == 0) return "Home";
  if (strcmp(normalized, "EL09UO") == 0) return "Home";
  if (strcmp(normalized, "EL19EO") == 0) return "Palmetto SP";
  if (strcmp(normalized, "EL19FO") == 0) return "Palmetto SP";
  if (strcmp(normalized, "EL09RU") == 0) return "Guadalupe River SP";
  if (strcmp(normalized, "EL09SU") == 0) return "Guadalupe River SP";
  if (strcmp(normalized, "EL19DU") == 0) return "Lockhart SP";
  if (strcmp(normalized, "EL09SH") == 0) return "SA Missions";
  return "";
}

const char* potaForGrid(const char* locator) {
  char normalized[7] = {0};
  for (size_t i = 0; i < 6 && locator[i] != '\0'; ++i) {
    normalized[i] = static_cast<char>(toupper(static_cast<unsigned char>(locator[i])));
  }

  if (strcmp(normalized, "EL09UO") == 0) return "Schertz";
  if (strcmp(normalized, "EL19EO") == 0) return "US-3045";
  if (strcmp(normalized, "EL19FO") == 0) return "US-3045";
  if (strcmp(normalized, "EL09RU") == 0) return "US-3017";
  if (strcmp(normalized, "EL09SU") == 0) return "US-3017";
  if (strcmp(normalized, "EL19DU") == 0) return "US-3033";
  if (strcmp(normalized, "EL09SH") == 0) return "US-4568, US-0756";
  return "";
}

bool isLocationOverrideActive() {
  if (locationOverrideActive &&
      static_cast<long>(millis() - locationOverrideExpiresMs) >= 0) {
    locationOverrideActive = false;
  }
  return locationOverrideActive;
}

bool currentLocationValid() {
  return isLocationOverrideActive() || gps.location.isValid();
}

double currentLatitude() {
  return isLocationOverrideActive() ? locationOverrideLat : gps.location.lat();
}

double currentLongitude() {
  return isLocationOverrideActive() ? locationOverrideLon : gps.location.lng();
}

void resetDisplayState() {
  lastDisplayedYear = -1;
  lastDisplayedDay = -1;
  lastDisplayedHour = -1;
  lastDisplayedMinute = -1;
  lastDisplayedUsedLocationOverride = false;
  lastDisplayedLocator[0] = '\0';
}

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

void printPinLevel(const __FlashStringHelper* label, uint8_t pin) {
  Serial.print(label);
  Serial.print(F("="));
  Serial.print(pin);
  Serial.print(F("("));
  Serial.print(digitalRead(pin) == HIGH ? F("HIGH") : F("LOW"));
  Serial.print(F(") "));
}

void logDisplaySpiDebug(const __FlashStringHelper* phase) {
  Serial.print(F("[SPI] "));
  Serial.println(phase);
  Serial.print(F("[SPI] bus SCK="));
  Serial.print(8);
  Serial.print(F(" MISO="));
  Serial.print(9);
  Serial.print(F(" MOSI="));
  Serial.println(10);
  Serial.print(F("[SPI] epd "));
  printPinLevel(F("CS"), Pins::EPD_CS);
  printPinLevel(F("DC"), Pins::EPD_DC);
  printPinLevel(F("RST"), Pins::EPD_RST);
  printPinLevel(F("BUSY"), Pins::EPD_BUSY);
  Serial.println();
}

void runDisplaySelfTest() {
  Serial.println(F("[SPI] running e-paper self-test"));

  display.setFullWindow();

  Serial.println(F("[SPI] self-test: full black"));
  display.firstPage();
  do {
    display.fillScreen(GxEPD_BLACK);
  } while (display.nextPage());
  delay(750);

  Serial.println(F("[SPI] self-test: full white"));
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());
  delay(750);

  Serial.println(F("[SPI] self-test: border and checker"));
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(4, 4, display.width() - 8, display.height() - 8, GxEPD_BLACK);
    for (int16_t y = 16; y < display.height() - 16; y += 16) {
      for (int16_t x = 16; x < display.width() - 16; x += 16) {
        if (((x + y) / 16) % 2 == 0) {
          display.fillRect(x, y, 8, 8, GxEPD_BLACK);
        }
      }
    }
  } while (display.nextPage());
  delay(1000);

  display.hibernate();
  Serial.println(F("[SPI] e-paper self-test complete"));
}

void initializeDisplay() {
  logDisplaySpiDebug(F("before SPI.begin"));
  SPI.begin();
  logDisplaySpiDebug(F("after SPI.begin"));
  Serial.println(F("[SPI] calling display.init"));
  display.init(115200, true, 2, false);
  logDisplaySpiDebug(F("after display.init"));
  display.setRotation(0);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();
  Serial.println(F("[SPI] display initialized"));
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

void drawTemperatureIcon(int16_t x, int16_t y) {
  display.drawRoundRect(x + 3, y, 6, 16, 3, GxEPD_BLACK);
  display.fillRect(x + 5, y + 3, 2, 8, GxEPD_BLACK);
  display.fillCircle(x + 6, y + 18, 5, GxEPD_BLACK);
  display.fillCircle(x + 6, y + 18, 2, GxEPD_WHITE);
}

uint8_t gpsLockQuality() {
  if (!gps.location.isValid() || !gps.date.isValid() || !gps.time.isValid()) {
    return 0;
  }

  const unsigned long satellites = gps.satellites.isValid() ? gps.satellites.value() : 0UL;
  const bool fresh =
    gps.location.age() <= GPS_DATA_FRESH_MS &&
    gps.date.age() <= GPS_DATA_FRESH_MS &&
    gps.time.age() <= GPS_DATA_FRESH_MS;

  if (!fresh || satellites < 4) {
    return 1;
  }
  if (satellites < 8) {
    return 2;
  }
  return 3;
}

void drawGpsLockQualityIcon(int16_t x, int16_t baselineY, uint8_t quality) {
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t barX = x + (i * 4);
    const int16_t barHeight = 4 + (i * 3);
    const int16_t barY = baselineY - barHeight;
    display.drawRect(barX, barY, 3, barHeight, GxEPD_BLACK);
    if (quality > i) {
      display.fillRect(barX, barY, 3, barHeight, GxEPD_BLACK);
    }
  }
}

float readTemperatureF() {
  tempSensors.requestTemperatures();
  return tempSensors.getTempFByIndex(0);
}

float pollTemperatureF() {
  cachedTemperatureF = readTemperatureF();
  return cachedTemperatureF;
}

void formatTemperature(char* buffer, size_t bufferSize, float temperatureF) {
  if (temperatureF < MIN_REASONABLE_TEMP_F) {
    snprintf(buffer, bufferSize, "??");
    return;
  }

  snprintf(buffer, bufferSize, "%dF", static_cast<int>(temperatureF + (temperatureF >= 0.0f ? 0.5f : -0.5f)));
}

void drawDisplayHeader(const char* timeLine,
                       const GFXfont* timeFont,
                       const char* dateLine,
                       const GFXfont* dateFont,
                       const char* potaLine) {
  const GFXfont* potaFont = &FreeSans12pt7b;
  drawCenteredText(timeLine, timeFont, display.width() / 2, 24);
  drawCenteredText(dateLine, dateFont, display.width() / 2, 59);
  if (potaLine[0] != '\0') {
    drawCenteredText(potaLine, potaFont, display.width() / 2, 89);
    display.fillRect(8, 102, display.width() - 16, 2, GxEPD_BLACK);
    return;
  }
  display.fillRect(8, 79, display.width() - 16, 2, GxEPD_BLACK);
}

void drawDisplayLocationBlock(const char* locator,
                              const char* countyLine,
                              const char* potaLine,
                              const char* parkLine) {
  const GFXfont* locatorFont = &FreeSans9pt7b;
  const GFXfont* locationFont = &FreeSans9pt7b;
  int16_t rowY = potaLine[0] != '\0' ? 122 : 110;

  display.setFont(locatorFont);
  display.setCursor(8, rowY);
  display.println(locator);
  rowY += 18;

  display.setFont(locationFont);
  if (countyLine[0] != '\0') {
    display.setCursor(8, rowY);
    display.println(countyLine);
    rowY += 18;
  }
  if (parkLine[0] != '\0') {
    display.setCursor(8, rowY);
    display.println(parkLine);
  }
}

void drawDisplayFooter(const char* footerLine, const GFXfont* tempFont, const char* temperatureLine) {
  const GFXfont* footerFont = &FreeSans9pt7b;

  drawGpsLockQualityIcon(8, 194, gpsLockQuality());
  display.setFont(footerFont);
  display.setCursor(24, 194);
  display.print(footerLine);

  if (logGpsSentences) {
    display.setFont();
    display.setCursor(128, 176);
    display.print("NMEA");
  }

  drawTemperatureIcon(154, 171);
  display.setFont(tempFont);
  display.setCursor(168, 188);
  display.print(temperatureLine);
}

void finishDisplayUpdate(bool fullRefresh) {
  if (fullRefresh) {
    minuteUpdatesSinceFullRefresh = 0;
    display.hibernate();
    return;
  }

  if (minuteUpdatesSinceFullRefresh < 255) {
    ++minuteUpdatesSinceFullRefresh;
  }
  display.powerOff();
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
  const GFXfont* font = &FreeSansBold12pt7b;

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawCenteredText("Acquiring GPS", font, display.width() / 2, display.height() / 2);
  } while (display.nextPage());
  display.hibernate();
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

bool hasFreshGpsTime() {
  return gps.date.isValid() &&
         gps.time.isValid() &&
         gps.date.age() <= GPS_DATA_FRESH_MS &&
         gps.time.age() <= GPS_DATA_FRESH_MS;
}

void consumeGpsData() {
  while (Serial1.available() > 0) {
    const char c = static_cast<char>(Serial1.read());
    gps.encode(c);

    if (logGpsSentences) {
      Serial.write(c);
    }
  }
}

void waitForFreshGpsTime() {
  unsigned long lastStatusMs = 0;

  while (!hasFreshGpsTime()) {
    consumeGpsData();

    const unsigned long nowMs = millis();
    if (nowMs - lastStatusMs >= GPS_LOCK_STATUS_MS) {
      Serial.print(F("Waiting for GPS time"));
      Serial.print(F(" sats="));
      Serial.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
      Serial.print(F(" loc="));
      Serial.print(gps.location.isValid() ? F("Y") : F("N"));
      Serial.print(F(" date="));
      Serial.print(gps.date.isValid() ? F("Y") : F("N"));
      Serial.print(F(" time="));
      Serial.print(gps.time.isValid() ? F("Y") : F("N"));
      if (gps.date.isValid() && gps.time.isValid()) {
        Serial.print(F(" gps_utc="));
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
        Serial.print(gps.date.day());
        Serial.print(F(" "));
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
        Serial.print(gps.time.second());
      }
      Serial.println();
      lastStatusMs = nowMs;
    }

    delay(10);
  }
  Serial.println(F("GPS time is locked."));
}

void printState() {
  const char* locator = currentLocationValid()
    ? get_mh(currentLatitude(), currentLongitude(), 6)
    : "------";

  Serial.println(F("state"));
  Serial.print(F("latitude: "));
  if (currentLocationValid()) {
    Serial.println(currentLatitude(), 6);
  } else {
    Serial.println(F("unavailable"));
  }

  Serial.print(F("longitude: "));
  if (currentLocationValid()) {
    Serial.println(currentLongitude(), 6);
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

  Serial.print(F("location override: "));
  Serial.println(isLocationOverrideActive() ? F("active") : F("inactive"));
}

void setLocationOverride(double latitude, double longitude) {
  locationOverrideLat = latitude;
  locationOverrideLon = longitude;
  locationOverrideExpiresMs = millis() + LOCATION_OVERRIDE_MS;
  locationOverrideActive = true;
  resetDisplayState();
  Serial.print(F("Location override set to "));
  Serial.print(latitude, 6);
  Serial.print(F(", "));
  Serial.print(longitude, 6);
  Serial.println(F(" for 5 minutes."));
}

bool parseLocationOverrideCommand(const char* command) {
  if (strncmp(command, "location ", 9) != 0) {
    return false;
  }

  const char* payload = command + 9;
  const char* comma = strchr(payload, ',');
  if (comma == nullptr) {
    Serial.println(F("Usage: location LAT, LON"));
    return true;
  }

  char latitudeText[24] = {0};
  char longitudeText[24] = {0};
  const size_t latitudeLength = static_cast<size_t>(comma - payload);
  if (latitudeLength == 0 || latitudeLength >= sizeof(latitudeText)) {
    Serial.println(F("Usage: location LAT, LON"));
    return true;
  }

  memcpy(latitudeText, payload, latitudeLength);
  latitudeText[latitudeLength] = '\0';

  const char* longitudeStart = comma + 1;
  while (*longitudeStart == ' ') {
    ++longitudeStart;
  }

  if (*longitudeStart == '\0' || strlen(longitudeStart) >= sizeof(longitudeText)) {
    Serial.println(F("Usage: location LAT, LON"));
    return true;
  }

  strcpy(longitudeText, longitudeStart);

  char* latitudeEnd = nullptr;
  char* longitudeEnd = nullptr;
  const double latitude = strtod(latitudeText, &latitudeEnd);
  const double longitude = strtod(longitudeText, &longitudeEnd);
  if (latitudeEnd == latitudeText || longitudeEnd == longitudeText) {
    Serial.println(F("Usage: location LAT, LON"));
    return true;
  }

  while (*latitudeEnd == ' ') {
    ++latitudeEnd;
  }
  while (*longitudeEnd == ' ') {
    ++longitudeEnd;
  }

  if (*latitudeEnd != '\0' || *longitudeEnd != '\0') {
    Serial.println(F("Usage: location LAT, LON"));
    return true;
  }

  setLocationOverride(latitude, longitude);
  updateDisplay(true);
  return true;
}

void handleSerialCommand(const char* command) {
  if (strcmp(command, "help") == 0) {
    Serial.println(F("Available commands:"));
    Serial.println(F("  help      - list serial commands"));
    Serial.println(F("  state     - print current GPS and clock state"));
    Serial.println(F("  nmea      - toggle raw NMEA sentence logging"));
    Serial.println(F("  temp      - poll the temperature sensor and refresh the display"));
    Serial.println(F("  epdtest   - run the e-paper self-test"));
    Serial.println(F("  restart   - restart the device"));
    Serial.println(F("  location LAT, LON - override GPS location for 5 minutes"));
    return;
  }

  if (strcmp(command, "state") == 0) {
    printState();
    return;
  }

  if (strcmp(command, "nmea") == 0) {
    logGpsSentences = !logGpsSentences;
    saveSettings();
    Serial.print(F("NMEA logging "));
    Serial.println(logGpsSentences ? F("enabled") : F("disabled"));
    updateDisplay(true);
    return;
  }

  if (strcmp(command, "restart") == 0) {
    Serial.println(F("Restarting device..."));
    Serial.flush();
    NVIC_SystemReset();
    return;
  }

  if (strcmp(command, "temp") == 0) {
    char temperatureLine[8] = {0};
    formatTemperature(temperatureLine, sizeof(temperatureLine), pollTemperatureF());
    Serial.print(F("temperature: "));
    Serial.println(temperatureLine);
    updateDisplay(true);
    return;
  }

  if (strcmp(command, "epdtest") == 0) {
    runDisplaySelfTest();
    updateDisplay(true);
    return;
  }

  if (parseLocationOverrideCommand(command)) {
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

void updateDisplay(bool forceFullRefresh) {
  const char* locator = currentLocationValid()
    ? get_mh(currentLatitude(), currentLongitude(), 6)
    : "";

  char timeLine[16] = {0};
  char dateLine[32] = {0};
  char countyLine[40] = {0};
  char potaLine[40] = {0};
  char parkLine[40] = {0};
  char footerLine[32] = {0};
  char temperatureLine[8] = {0};
  const char* county = countyForGrid(locator);
  const char* pota = potaForGrid(locator);
  const char* park = parkForGrid(locator);

  snprintf(timeLine, sizeof(timeLine), "%02d:%02d", hour(), minute());
  snprintf(dateLine, sizeof(dateLine), "%02d %s %04d", day(), MONTH_NAMES[month() - 1], year());
  snprintf(countyLine, sizeof(countyLine), "%s", county);
  snprintf(potaLine, sizeof(potaLine), "%s", pota);
  snprintf(parkLine, sizeof(parkLine), "%s", park);
  switch (gpsLockQuality()) {
    case 0:
      snprintf(footerLine, sizeof(footerLine), "No Lock");
      break;
    case 1:
      snprintf(footerLine, sizeof(footerLine), "Weak");
      break;
    case 2:
      snprintf(footerLine, sizeof(footerLine), "OK");
      break;
    default:
      snprintf(footerLine, sizeof(footerLine), "Good");
      break;
  }

  const GFXfont* timeFont = &FreeMonoBold24pt7b;
  if (!textFits(timeFont, timeLine, display.width() - 4, 44)) {
    timeFont = &FreeMonoBold18pt7b;
  }
  if (!textFits(timeFont, timeLine, display.width() - 4, 44)) {
    timeFont = &FreeMonoBold12pt7b;
  }

  const GFXfont* dateFont = &FreeSans18pt7b;
  if (!textFits(dateFont, dateLine, display.width() - 4, 24)) {
    dateFont = &FreeSans12pt7b;
  }

  const GFXfont* tempFont = &FreeSansBold12pt7b;
  const bool usePartialRefresh =
    !forceFullRefresh &&
    GxEPD2_DRIVER_CLASS::hasFastPartialUpdate &&
    minuteUpdatesSinceFullRefresh < (FULL_REFRESH_INTERVAL_MINUTES - 1);
  const float temperatureF = usePartialRefresh ? cachedTemperatureF : pollTemperatureF();
  formatTemperature(temperatureLine, sizeof(temperatureLine), temperatureF);

  if (usePartialRefresh) {
    display.setPartialWindow(0, 0, display.width(), PARTIAL_HEADER_HEIGHT);
    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
      drawDisplayHeader(timeLine, timeFont, dateLine, dateFont, potaLine);
    } while (display.nextPage());

    display.setPartialWindow(0, PARTIAL_FOOTER_Y, display.width(), PARTIAL_FOOTER_HEIGHT);
    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
      drawDisplayFooter(footerLine, tempFont, temperatureLine);
    } while (display.nextPage());

    finishDisplayUpdate(false);
  } else {
    display.setFullWindow();
    display.firstPage();
    do {
      display.fillScreen(GxEPD_WHITE);
      drawDisplayHeader(timeLine, timeFont, dateLine, dateFont, potaLine);
      drawDisplayLocationBlock(locator, countyLine, potaLine, parkLine);
      drawDisplayFooter(footerLine, tempFont, temperatureLine);
    } while (display.nextPage());

    finishDisplayUpdate(true);
  }

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
  Serial.print(usePartialRefresh ? F("  PARTIAL ") : F("  FULL "));
  Serial.print(F("  Grid "));
  Serial.print(locator);
  Serial.print(F("  Sats "));
  Serial.println(gps.satellites.value());
}

bool shouldUpdateDisplayForCurrentMinute() {
  const bool usingLocationOverride = isLocationOverrideActive();
  const char* locator = currentLocationValid()
    ? get_mh(currentLatitude(), currentLongitude(), 6)
    : "------";
  const bool locatorChanged = strcmp(locator, lastDisplayedLocator) != 0;
  if (year() != lastDisplayedYear ||
      day() != lastDisplayedDay ||
      hour() != lastDisplayedHour ||
      minute() != lastDisplayedMinute ||
      locatorChanged ||
      usingLocationOverride != lastDisplayedUsedLocationOverride) {
    displayNeedsFullRefresh =
      usingLocationOverride != lastDisplayedUsedLocationOverride ||
      locatorChanged;
    lastDisplayedYear = year();
    lastDisplayedDay = day();
    lastDisplayedHour = hour();
    lastDisplayedMinute = minute();
    lastDisplayedUsedLocationOverride = usingLocationOverride;
    strncpy(lastDisplayedLocator, locator, sizeof(lastDisplayedLocator) - 1);
    lastDisplayedLocator[sizeof(lastDisplayedLocator) - 1] = '\0';
    return true;
  }

  return false;
}

void setup() {
  loadSettingsIntoRuntime();
  initializePins();
  initializeSerial();
  initializeDisplay();
  runDisplaySelfTest();
  tempSensors.begin();
  pollTemperatureF();
  showAcquiringGps();
  waitForFreshGpsTime();
  syncSystemTimeFromGps();
  lastGpsResyncMs = millis();
  updateDisplay(true);
  shouldUpdateDisplayForCurrentMinute();
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

  if (shouldUpdateDisplayForCurrentMinute()) {
    updateDisplay(displayNeedsFullRefresh);
    displayNeedsFullRefresh = false;
  }
}
