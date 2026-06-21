#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <INA226_WE.h>
#include <RTClib.h>
#include <SD.h>

// ESP32-C3 Super Mini pin assignments
// GPIO8/9 are strapping pins (GPIO9 = BOOT), use GPIO4/5 for I2C instead
#define SDA_PIN  4
#define SCL_PIN  5
#define SD_MOSI  6
#define SD_MISO  7
#define SD_SCK   10
#define SD_CS    3

#define INA226_ADDR  0x40
#define OLED_ADDR    0x3C
#define SCREEN_W     128
#define SCREEN_H     64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
INA226_WE ina226(INA226_ADDR);
RTC_DS3231 rtc;

bool sdAvailable = false;

void setup() {
  Serial.begin(115200);
  // Wait for USB CDC to connect (ESP32-C3 uses native USB), timeout 3s
  while (!Serial && millis() < 3000);

  // Wire must be initialized before any I2C device
  Wire.begin(SDA_PIN, SCL_PIN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("INA226 v1");
    display.println("Initializing...");
    display.display();
    Serial.println("OLED OK");
  }

  ina226.init();
  Serial.println("INA226 OK");

  if (!rtc.begin()) {
    Serial.println("DS3231 init failed");
  } else {
    // If RTC lost power (dead coin cell), sync to compile-time
    if (rtc.lostPower()) {
      Serial.println("RTC lost power - syncing to compile time");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    Serial.println("DS3231 OK");
  }

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("SD card init failed");
    sdAvailable = false;
  } else {
    sdAvailable = true;
    Serial.println("SD card OK");
    if (!SD.exists("/data.csv")) {
      File f = SD.open("/data.csv", FILE_WRITE);
      if (f) {
        f.println("datetime,bus_V,load_V,shunt_mV,current_mA,power_mW,battery_pct,overflow");
        f.close();
      }
    }
  }

  Serial.println("INA226 ready - sampling started");
  Serial.println();
}

void loop() {
  for (int i = 0; i < 5; i++) {
    continuousSampling();
    delay(3000);
  }

  Serial.println("INA226 power down for 7s");
  ina226.powerDown();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 24);
  display.println("  INA226 sleeping...");
  display.display();

  for (int i = 0; i < 7; i++) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("\nINA226 power up");
  Serial.println();
  ina226.powerUp();
}

String getTimestamp(const DateTime &dt) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());
  return String(buf);
}

void displayOLED(const DateTime &dt,
                 float busV, float loadV, float shuntMv,
                 float currMa, float powerMw, float batPct,
                 bool overflow) {
  char buf[22];
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Row 0 (y=0):  date and time
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());
  display.setCursor(0, 0);  display.print(buf);

  // Row 1 (y=8):  bus voltage
  snprintf(buf, sizeof(buf), "Bus:  %6.3f V", busV);
  display.setCursor(0, 8);  display.print(buf);

  // Row 2 (y=16): load voltage
  snprintf(buf, sizeof(buf), "Load: %6.3f V", loadV);
  display.setCursor(0, 16); display.print(buf);

  // Row 3 (y=24): shunt voltage
  snprintf(buf, sizeof(buf), "Shnt: %6.3f mV", shuntMv);
  display.setCursor(0, 24); display.print(buf);

  // Row 4 (y=32): current
  snprintf(buf, sizeof(buf), "Curr: %6.2f mA", currMa);
  display.setCursor(0, 32); display.print(buf);

  // Row 5 (y=40): power
  snprintf(buf, sizeof(buf), "Powr: %6.1f mW", powerMw);
  display.setCursor(0, 40); display.print(buf);

  // Row 6 (y=48): battery percentage
  snprintf(buf, sizeof(buf), "Bat:  %5.1f %%", batPct);
  display.setCursor(0, 48); display.print(buf);

  // Row 7 (y=56): status
  display.setCursor(0, 56);
  display.print(overflow ? "Status: OVERFLOW!" : "Status: OK");

  display.display();
}

void logToSD(const String &timestamp,
             float busV, float loadV, float shuntMv,
             float currMa, float powerMw, float batPct,
             bool overflow) {
  if (!sdAvailable) return;
  File f = SD.open("/data.csv", FILE_APPEND);
  if (!f) {
    Serial.println("SD write error");
    return;
  }
  f.print(timestamp); f.print(",");
  f.print(busV, 3);   f.print(",");
  f.print(loadV, 3);  f.print(",");
  f.print(shuntMv, 3); f.print(",");
  f.print(currMa, 2); f.print(",");
  f.print(powerMw, 2); f.print(",");
  f.print(batPct, 1); f.print(",");
  f.println(overflow ? 1 : 0);
  f.close();
}

void continuousSampling() {
  ina226.readAndClearFlags();
  float shuntMv = ina226.getShuntVoltage_mV();
  float busV    = ina226.getBusVoltage_V();
  float currMa  = ina226.getCurrent_mA();
  float powerMw = ina226.getBusPower();
  float loadV   = busV + (shuntMv / 1000.0);
  bool  overflow = ina226.overflow;
  float batPct  = (busV / 0.07) * 10.0;

  DateTime now = rtc.now();
  String ts = getTimestamp(now);

  Serial.println("=== " + ts + " ===");
  Serial.print("Bus Voltage  [V]:  "); Serial.println(busV, 3);
  Serial.print("Load Voltage [V]:  "); Serial.println(loadV, 3);
  Serial.print("Shunt Voltage[mV]: "); Serial.println(shuntMv, 3);
  Serial.print("Current      [mA]: "); Serial.println(currMa, 2);
  Serial.print("Power        [mW]: "); Serial.println(powerMw, 2);
  Serial.print("Battery      [%]:  "); Serial.println(batPct, 1);
  Serial.println(overflow ? "Status: OVERFLOW!" : "Status: OK");
  Serial.println();

  displayOLED(now, busV, loadV, shuntMv, currMa, powerMw, batPct, overflow);
  logToSD(ts, busV, loadV, shuntMv, currMa, powerMw, batPct, overflow);
}
