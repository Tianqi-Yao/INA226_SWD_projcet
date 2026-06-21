#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <INA226_WE.h>
#include <RTClib.h>
#include <SD.h>

// ESP32-C3 Super Mini pin assignments
// I2C: GPIO5/6 (free, no special functions)
// SPI SD: GPIO1-4 in a row for easy soldering
// GPIO2 is a strapping pin (LOW at boot enables USB-JTAG). CLK floats before SPI.begin()
// so the boot level is indeterminate — add a 10k pull-up to 3V3 on GPIO2 on your PCB.
#define SDA_PIN  5
#define SCL_PIN  6
#define SD_MISO  1
#define SD_SCK   2
#define SD_MOSI  3
#define SD_CS    4

#define INA226_ADDR  0x40
#define OLED_ADDR    0x3C
#define SCREEN_W     128
#define SCREEN_H     64

// LiPo single-cell voltage range for battery % estimation.
// Adjust BATT_MIN_V / BATT_MAX_V to match your actual cell chemistry.
#define BATT_MIN_V  3.0f
#define BATT_MAX_V  4.2f

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
INA226_WE ina226(INA226_ADDR);
RTC_DS3231 rtc;

bool sdAvailable      = false;
bool displayAvailable = false;
bool rtcAvailable     = false;
bool inaAvailable     = false;

void setup() {
  Serial.begin(115200);
  // Wait for USB CDC to connect (ESP32-C3 uses native USB), timeout 3s
  while (!Serial && millis() < 3000);

  // Wire must be initialized before any I2C device
  Wire.begin(SDA_PIN, SCL_PIN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
  } else {
    displayAvailable = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("QuantumVolt");
    display.println("Initializing...");
    display.display();
    Serial.println("OLED OK");
  }

  if (!ina226.init()) {
    Serial.println("INA226 init failed - check wiring and I2C address");
  } else {
    inaAvailable = true;
    Serial.println("INA226 OK");
  }

  if (!rtc.begin()) {
    Serial.println("DS3231 init failed - timestamps disabled");
  } else {
    rtcAvailable = true;
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
  } else {
    sdAvailable = true;
    Serial.println("SD card OK");
    if (!SD.exists("/data.csv")) {
      File f = SD.open("/data.csv", FILE_WRITE);
      if (f) {
        f.println("datetime,bus_V,supply_V,shunt_mV,current_mA,power_mW,battery_pct,overflow");
        f.close();
      }
    }
  }

  Serial.println("QuantumVolt ready - sampling started");
  Serial.println();
}

void loop() {
  for (int i = 0; i < 5; i++) {
    continuousSampling();
    delay(3000);
  }

  Serial.println("INA226 power down for 7s");
  if (inaAvailable) ina226.powerDown();

  if (displayAvailable) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 24);
    display.println("  INA226 sleeping...");
    display.display();
  }

  for (int i = 0; i < 7; i++) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("\nINA226 power up");
  Serial.println();
  if (inaAvailable) {
    ina226.powerUp();
    delay(10);  // allow first conversion to complete before sampling
  }
}

String getTimestamp(const DateTime &dt) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());
  return String(buf);
}

void displayOLED(const DateTime &dt, bool dtValid,
                 float busV, float supplyV, float shuntMv,
                 float currMa, float powerMw, float batPct,
                 bool overflow) {
  char buf[22];
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Row 0 (y=0): date and time (or placeholder when RTC unavailable)
  if (dtValid) {
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             dt.year(), dt.month(), dt.day(),
             dt.hour(), dt.minute(), dt.second());
  } else {
    snprintf(buf, sizeof(buf), "No RTC");
  }
  display.setCursor(0, 0);  display.print(buf);

  // Row 1 (y=8):  bus voltage (= load-side voltage, measured at IN-)
  snprintf(buf, sizeof(buf), "Bus:  %6.3f V", busV);
  display.setCursor(0, 8);  display.print(buf);

  // Row 2 (y=16): supply voltage (= busV + shunt drop, reconstructed IN+ side)
  snprintf(buf, sizeof(buf), "Sup:  %6.3f V", supplyV);
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
             float busV, float supplyV, float shuntMv,
             float currMa, float powerMw, float batPct,
             bool overflow) {
  if (!sdAvailable) return;
  File f = SD.open("/data.csv", FILE_APPEND);
  if (!f) {
    Serial.println("SD write error");
    return;
  }
  f.print(timestamp); f.print(",");
  f.print(busV, 3);     f.print(",");
  f.print(supplyV, 3);  f.print(",");
  f.print(shuntMv, 3);  f.print(",");
  f.print(currMa, 2);   f.print(",");
  f.print(powerMw, 2);  f.print(",");
  f.print(batPct, 1);   f.print(",");
  f.println(overflow ? 1 : 0);
  f.close();
}

void continuousSampling() {
  if (!inaAvailable) return;

  ina226.readAndClearFlags();
  float shuntMv  = ina226.getShuntVoltage_mV();
  float busV     = ina226.getBusVoltage_V();
  float currMa   = ina226.getCurrent_mA();
  float powerMw  = ina226.getBusPower();
  // busV is the load-side voltage (measured at IN-).
  // Adding the shunt drop reconstructs the supply-side voltage (at IN+).
  float supplyV  = busV + (shuntMv / 1000.0f);
  bool  overflow = ina226.overflow;
  // Linear estimate across the usable LiPo window; clamped to [0, 100].
  float batPct   = constrain((busV - BATT_MIN_V) / (BATT_MAX_V - BATT_MIN_V) * 100.0f,
                             0.0f, 100.0f);

  DateTime now(0);  // default-init to epoch; overwritten below if RTC is available
  String ts;
  if (rtcAvailable) {
    now = rtc.now();
    ts  = getTimestamp(now);
  } else {
    ts = "NO-RTC";
  }

  Serial.println("=== " + ts + " ===");
  Serial.print("Bus Voltage    [V]:  "); Serial.println(busV, 3);
  Serial.print("Supply Voltage [V]:  "); Serial.println(supplyV, 3);
  Serial.print("Shunt Voltage  [mV]: "); Serial.println(shuntMv, 3);
  Serial.print("Current        [mA]: "); Serial.println(currMa, 2);
  Serial.print("Power          [mW]: "); Serial.println(powerMw, 2);
  Serial.print("Battery        [%]:  "); Serial.println(batPct, 1);
  Serial.println(overflow ? "Status: OVERFLOW!" : "Status: OK");
  Serial.println();

  if (displayAvailable) {
    displayOLED(now, rtcAvailable, busV, supplyV, shuntMv,
                currMa, powerMw, batPct, overflow);
  }
  logToSD(ts, busV, supplyV, shuntMv, currMa, powerMw, batPct, overflow);
}
