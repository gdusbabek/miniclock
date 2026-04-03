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
constexpr unsigned long LOCATION_OVERRIDE_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long GPS_RESYNC_MS = 60UL * 1000UL;
constexpr unsigned long GPS_DATA_FRESH_MS = 2000UL;
constexpr unsigned long GPS_LOCK_STATUS_MS = 1000UL;
constexpr size_t SERIAL_COMMAND_BUFFER_SIZE = 64;

GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)> display(
  GxEPD2_DRIVER_CLASS(Pins::EPD_CS, Pins::EPD_DC, Pins::EPD_RST, Pins::EPD_BUSY)
);

TinyGPSPlus gps;
OneWire oneWire(Pins::ONE_WIRE);
DallasTemperature tempSensors(&oneWire);

unsigned long lastGpsResyncMs = 0;
int lastDisplayedYear = -1;
int lastDisplayedDay = -1;
int lastDisplayedHour = -1;
int lastDisplayedMinute = -1;
bool lastDisplayedUsedLocationOverride = false;
char serialCommandBuffer[SERIAL_COMMAND_BUFFER_SIZE] = {0};
size_t serialCommandLength = 0;
bool locationOverrideActive = false;
double locationOverrideLat = 0.0;
double locationOverrideLon = 0.0;
unsigned long locationOverrideExpiresMs = 0;

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

  if (strcmp(normalized, "L09UO") == 0) return "Home";
  if (strcmp(normalized, "EL09UO") == 0) return "Home";
  if (strcmp(normalized, "EL19EO") == 0) return "3045 Palmetto SP";
  if (strcmp(normalized, "EL19FO") == 0) return "3045 Palmetto SP";
  if (strcmp(normalized, "EL09RU") == 0) return "3017 Guadalupe Riv";
  if (strcmp(normalized, "EL09SU") == 0) return "3017 Guadalupe Riv";
  if (strcmp(normalized, "EL19DU") == 0) return "3033 Lockhart SP";
  if (strcmp(normalized, "EL09SH") == 0) return "4568, 0756 Missions";
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

void drawTemperatureIcon(int16_t x, int16_t y) {
  display.drawRoundRect(x + 3, y, 6, 16, 3, GxEPD_BLACK);
  display.fillRect(x + 5, y + 3, 2, 8, GxEPD_BLACK);
  display.fillCircle(x + 6, y + 18, 5, GxEPD_BLACK);
  display.fillCircle(x + 6, y + 18, 2, GxEPD_WHITE);
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
  updateDisplay();
  return true;
}

void handleSerialCommand(const char* command) {
  if (strcmp(command, "state") == 0) {
    printState();
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

void updateDisplay() {
  const char* locator = currentLocationValid()
    ? get_mh(currentLatitude(), currentLongitude(), 6)
    : "------";

  char timeLine[16] = {0};
  char dateLine[32] = {0};
  char line1[40] = {0};
  char line2[40] = {0};
  char line3[40] = {0};
  char footerLine[32] = {0};
  const char* county = countyForGrid(locator);
  const char* park = parkForGrid(locator);

  snprintf(timeLine, sizeof(timeLine), "%02d:%02d", hour(), minute());
  snprintf(dateLine, sizeof(dateLine), "%02d %s %04d", day(), MONTH_NAMES[month() - 1], year());
  snprintf(line1, sizeof(line1), "%s", locator);
  snprintf(line2, sizeof(line2), "%s", county);
  snprintf(line3, sizeof(line3), "%s", park);
  snprintf(footerLine, sizeof(footerLine), "GPS lock - %lu sats", gps.satellites.isValid() ? gps.satellites.value() : 0UL);

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

  const GFXfont* locationFont = &FreeSans9pt7b;
  const GFXfont* tempFont = &FreeSansBold12pt7b;

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawCenteredText(timeLine, timeFont, display.width() / 2, 24);
    drawCenteredText(dateLine, dateFont, display.width() / 2, 59);
    display.drawLine(8, 76, display.width() - 8, 76, GxEPD_BLACK);

    display.setFont(locationFont);
    display.setCursor(8, 96);
    display.println(line1);
    if (line2[0] != '\0') {
      display.setCursor(8, 112);
      display.println(line2);
    }
    if (line3[0] != '\0') {
      display.setCursor(8, 128);
      display.println(line3);
    }

    display.setCursor(8, 190);
    display.print(footerLine);

    drawTemperatureIcon(154, 171);
    display.setFont(tempFont);
    display.setCursor(168, 188);
    display.print("77F");
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

bool shouldUpdateDisplayForCurrentMinute() {
  const bool usingLocationOverride = isLocationOverrideActive();
  if (year() != lastDisplayedYear ||
      day() != lastDisplayedDay ||
      hour() != lastDisplayedHour ||
      minute() != lastDisplayedMinute ||
      usingLocationOverride != lastDisplayedUsedLocationOverride) {
    lastDisplayedYear = year();
    lastDisplayedDay = day();
    lastDisplayedHour = hour();
    lastDisplayedMinute = minute();
    lastDisplayedUsedLocationOverride = usingLocationOverride;
    return true;
  }

  return false;
}

void setup() {
  initializePins();
  initializeSerial();
  initializeDisplay();
  showAcquiringGps();
  // tempSensors.begin();
  waitForFreshGpsLock();
  syncSystemTimeFromGps();
  lastGpsResyncMs = millis();
  updateDisplay();
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
    updateDisplay();
  }
}
