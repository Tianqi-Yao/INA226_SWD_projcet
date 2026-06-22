#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
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

#define INA226_ADDR  0x44
#define OLED_ADDR    0x3C
#define SCREEN_W     128
#define SCREEN_H     64

#define BOOT_PIN           9        // ESP32-C3 Super Mini onboard BOOT button
#define DISPLAY_TIMEOUT_MS 15000UL  // OLED auto-off after 15s of inactivity
#define LONG_PRESS_MS      3000UL   // Hold duration to trigger WiFi AP mode

// ESP32-C3 Super Mini onboard LED is active LOW (LOW = on, HIGH = off).
// If your board uses active HIGH, swap the two values below.
#define LED_PIN 8
#define LED_ON  LOW
#define LED_OFF HIGH

#define WIFI_SSID "SWD_INA226"
#define WIFI_PASS "12345678"

// ── Peripherals ───────────────────────────────────────────────────────────────
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
INA226_WE        ina226(INA226_ADDR);
RTC_DS3231       rtc;
WebServer        server(80);

// ── Runtime flags ─────────────────────────────────────────────────────────────
bool          sdAvailable      = false;
bool          sdWriteOk        = false;
bool          displayAvailable = false;
bool          rtcAvailable     = false;
bool          inaAvailable     = false;
bool          isWiFiMode       = false;
bool          wifiExitReq      = false;
unsigned long displayWakeMs    = 0;

// ── Button state ──────────────────────────────────────────────────────────────
static bool          btnLastReading  = HIGH;
static unsigned long btnDebounceTime = 0;
static unsigned long btnPressStart   = 0;
static bool          btnLongHandled  = false;

// ── SD log state ─────────────────────────────────────────────────────────────
String logLastPath  = "";  // tracks current daily file; reset forces header re-creation

// ── Last sampled values (served to web page) ──────────────────────────────────
float  lastBusV     = 0;
float  lastCurrMa   = 0;
float  lastPowerMw  = 0;
bool   lastOverflow = false;
String lastTs       = "--";

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
void continuousSampling();

// ─────────────────────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);

  pinMode(BOOT_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

  Wire.begin(SDA_PIN, SCL_PIN);

  // I2C bus scan — helps diagnose address mismatches
  Serial.println("I2C scan:");
  int i2cFound = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("  0x"); if (addr < 16) Serial.print("0");
      Serial.print(addr, HEX);
      if (addr == 0x3C || addr == 0x3D) Serial.print(" (OLED)");
      if (addr >= 0x40 && addr <= 0x4F) Serial.print(" (INA226)");
      if (addr == 0x68 || addr == 0x69) Serial.print(" (DS3231)");
      Serial.println();
      i2cFound++;
    }
  }
  if (i2cFound == 0) Serial.println("  No devices found");
  Serial.println();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
  } else {
    displayAvailable = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("SWD_INA226");
    display.println("Initializing...");
    display.display();
    Serial.println("OLED OK");
    delay(2000);
    display.ssd1306_command(SSD1306_DISPLAYOFF);
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
    sdWriteOk   = true;
    Serial.println("SD card OK");
  }

  Serial.println("SWD_INA226 ready");
  Serial.println();
}

// ─────────────────────────────────────────────────────────────────────────────
// Button: debounced short/long press detection
//   Short press (< LONG_PRESS_MS) → wake OLED
//   Long press  (≥ LONG_PRESS_MS) → start WiFi AP
// ─────────────────────────────────────────────────────────────────────────────
void checkButtonEvents() {
  bool reading = digitalRead(BOOT_PIN);

  if (reading != btnLastReading) btnDebounceTime = millis();
  btnLastReading = reading;
  if (millis() - btnDebounceTime < 30) return;  // debounce

  if (reading == LOW) {
    if (btnPressStart == 0) {
      btnPressStart  = millis();
      if (btnPressStart == 0) btnPressStart = 1;
      btnLongHandled = false;
    } else if (!btnLongHandled && millis() - btnPressStart >= LONG_PRESS_MS) {
      btnLongHandled = true;
      if (!isWiFiMode) {
        Serial.println("Long press → starting WiFi AP");
        isWiFiMode = startWifi();
      }
    }
  } else {
    if (btnPressStart > 0 && !btnLongHandled) {
      // Short press: wake OLED
      if (displayAvailable) {
        displayWakeMs = millis();
        display.ssd1306_command(SSD1306_DISPLAYON);
      }
    }
    btnPressStart  = 0;
    btnLongHandled = false;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Display helpers
// ─────────────────────────────────────────────────────────────────────────────
void checkDisplayTimeout() {
  if (displayAvailable && displayWakeMs > 0
      && millis() - displayWakeMs >= DISPLAY_TIMEOUT_MS) {
    displayWakeMs = 0;
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// LED helpers
// ─────────────────────────────────────────────────────────────────────────────
void updateLED() {
  bool anomaly = !sdAvailable || !sdWriteOk || !inaAvailable;
  if (anomaly) {
    digitalWrite(LED_PIN, (millis() / 500) % 2 == 0 ? LED_ON : LED_OFF);
  } else {
    digitalWrite(LED_PIN, LED_OFF);
  }
}

void blinkLEDWifi() {
  static unsigned long lastToggle = 0;
  static bool state = false;
  if (millis() - lastToggle >= 500) {
    state = !state;
    digitalWrite(LED_PIN, state ? LED_ON : LED_OFF);
    lastToggle = millis();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Web UI HTML
// ─────────────────────────────────────────────────────────────────────────────
static String htmlEscape(const String &s) {
  String r = s;
  r.replace("&", "&amp;");
  r.replace("<", "&lt;");
  r.replace(">", "&gt;");
  r.replace("\"", "&quot;");
  return r;
}
static String buildUI() {
  char busVBuf[10], currBuf[10], powrBuf[10];
  snprintf(busVBuf, sizeof(busVBuf), "%.3f", lastBusV);
  snprintf(currBuf, sizeof(currBuf), "%.2f",  lastCurrMa);
  snprintf(powrBuf, sizeof(powrBuf), "%.1f",  lastPowerMw);

  String sdStatus = !sdAvailable ? "NO CARD"
                  : sdWriteOk    ? "OK"
                                 : "WRITE FAIL";

  // Build file list from SD root (data_*.csv files only)
  String fileListHtml;
  if (sdAvailable) {
    File root = SD.open("/");
    if (root) {
      while (true) {
        File entry = root.openNextFile();
        if (!entry) break;
        String raw = String(entry.name());
        int sl = raw.lastIndexOf('/');
        String name = (sl >= 0) ? raw.substring(sl + 1) : raw;
        if (!entry.isDirectory() && name.startsWith("data_") && name.endsWith(".csv")) {
          String path = "/" + name;
          fileListHtml += "<li><a href='/download?path=" + htmlEscape(path) + "'>" + htmlEscape(name) + "</a>"
                       + " <button onclick=\"deleteFile('" + htmlEscape(path) + "')\">Delete</button></li>";
        }
        entry.close();
      }
      root.close();
    }
  }
  if (fileListHtml.isEmpty()) fileListHtml = "<li>No data files yet</li>";

  return R"rawliteral(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>SWD_INA226</title>
  <style>
    body{font-family:sans-serif;padding:20px;max-width:480px}
    h1{font-size:1.4em;margin-bottom:12px}
    p{margin:6px 0}
    .btns{margin-top:16px}
    button{padding:10px 16px;font-size:15px;margin:6px 6px 0 0;cursor:pointer}
  </style>
  <script>
    function syncRTC(){
      var t=new Date().toLocaleString('sv-SE').replace(' ','T');
      fetch('/rtc-sync',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({time:t})})
        .then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.text();})
        .then(function(m){alert(m);})
        .catch(function(e){alert('Sync failed: '+e.message);});
    }
    function exitWifi(){
      fetch('/exit',{method:'POST'})
        .then(function(){alert('WiFi mode exiting...');})
        .catch(function(){alert('Request failed');});
    }
    function deleteFile(path){
      if(!confirm('Delete '+path.split('/').pop()+'?')) return;
      fetch('/delete?path='+encodeURIComponent(path),{method:'POST'})
        .then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.text();})
        .then(function(m){alert(m);location.reload();})
        .catch(function(e){alert('Delete failed: '+e.message);});
    }
    function deleteAll(){
      if(!confirm('Delete ALL data files? This cannot be undone.')) return;
      fetch('/delete-all',{method:'POST'})
        .then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.text();})
        .then(function(m){alert(m);location.reload();})
        .catch(function(e){alert('Delete failed: '+e.message);});
    }
  </script>
</head>
<body>
  <h1>SWD_INA226</h1>
  <p><strong>Time:</strong> )rawliteral" + lastTs + R"rawliteral(</p>
  <p><strong>Bus Voltage:</strong> )rawliteral" + String(busVBuf) + R"rawliteral( V</p>
  <p><strong>Current:</strong> )rawliteral" + String(currBuf) + R"rawliteral( mA</p>
  <p><strong>Power:</strong> )rawliteral" + String(powrBuf) + R"rawliteral( mW</p>
  <p><strong>SD Card:</strong> )rawliteral" + sdStatus + R"rawliteral(</p>
  <p><strong>Status:</strong> )rawliteral" + String(lastOverflow ? "OVERFLOW" : "OK") + R"rawliteral(</p>
  <div class="btns">
    <button onclick="location.reload()">Refresh</button>
    <button onclick="syncRTC()">Sync RTC with Phone</button>
    <button onclick="exitWifi()">Exit WiFi</button>
  </div>
  <h2>Data Files</h2>
  <button onclick="deleteAll()" style="margin-bottom:8px">Delete All</button>
  <ul>
  )rawliteral" + fileListHtml + R"rawliteral(
  </ul>
</body>
</html>)rawliteral";
}

// ─────────────────────────────────────────────────────────────────────────────
// Web route handlers
// ─────────────────────────────────────────────────────────────────────────────
void handleRoot() {
  server.send(200, "text/html", buildUI());
}

void handleDownload() {
  if (!sdAvailable) { server.send(503, "text/plain", "SD card not ready"); return; }
  if (!server.hasArg("path")) { server.send(400, "text/plain", "Missing path parameter"); return; }

  String path = server.arg("path");
  // Only allow /data_*.csv files; reject directory traversal
  if (!path.startsWith("/data_") || !path.endsWith(".csv") || path.indexOf("..") >= 0) {
    server.send(403, "text/plain", "Path not allowed");
    return;
  }

  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) { if (f) f.close(); server.send(404, "text/plain", "File not found"); return; }

  String filename = path.substring(path.lastIndexOf('/') + 1);
  server.setContentLength(f.size());
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.send(200, "text/csv", "");
  uint8_t buf[512];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) break;
    server.sendContent((const char*)buf, n);
  }
  f.close();
}

void handleRTCSync() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "Missing body"); return; }
  String body = server.arg("plain");

  // Extract value of "time" key from {"time":"2026-06-21T12:30:45"}
  int keyIdx = body.indexOf("\"time\":");
  if (keyIdx < 0) { server.send(400, "text/plain", "Missing time field"); return; }
  int q1 = body.indexOf('"', keyIdx + 7);
  int q2 = (q1 >= 0) ? body.indexOf('"', q1 + 1) : -1;
  if (q1 < 0 || q2 <= q1) { server.send(400, "text/plain", "Invalid JSON format"); return; }
  String iso = body.substring(q1 + 1, q2);

  int y, mo, d, h, mi, s;
  if (sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
    server.send(400, "text/plain", "Time parse failed"); return;
  }
  // Validate year, month, and time fields before using mo as an array index
  if (y < 2020 || y > 2099 || mo < 1 || mo > 12 ||
      h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
    server.send(400, "text/plain", "Time values out of range"); return;
  }
  static const int daysInMonth[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  bool isLeap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  int  maxDay  = (mo == 2 && isLeap) ? 29 : daysInMonth[mo];
  if (d < 1 || d > maxDay) {
    server.send(400, "text/plain", "Time values out of range"); return;
  }
  if (rtcAvailable) {
    rtc.adjust(DateTime(y, mo, d, h, mi, s));
    Serial.println("RTC synchronized from browser");
    server.send(200, "text/plain", "RTC synchronized");
  } else {
    server.send(503, "text/plain", "RTC not available");
  }
}

void handleExit() {
  wifiExitReq = true;
  server.send(204);
  Serial.println("WiFi exit requested via web");
}

void handleDelete() {
  if (!sdAvailable) { server.send(503, "text/plain", "SD not ready"); return; }
  if (!server.hasArg("path")) { server.send(400, "text/plain", "Missing path"); return; }
  String path = server.arg("path");
  if (!path.startsWith("/data_") || !path.endsWith(".csv") || path.indexOf("..") >= 0) {
    server.send(403, "text/plain", "Path not allowed"); return;
  }
  if (!SD.exists(path)) { server.send(404, "text/plain", "File not found"); return; }
  if (SD.remove(path)) {
    if (logLastPath == path) logLastPath = "";  // force header re-creation if needed
    Serial.println("Deleted: " + path);
    server.send(200, "text/plain", "Deleted: " + path);
  } else {
    server.send(500, "text/plain", "Delete failed");
  }
}

void handleDeleteAll() {
  if (!sdAvailable) { server.send(503, "text/plain", "SD not ready"); return; }
  // Collect matching files first, then delete (avoid mutating dir while iterating)
  String toDelete[64];
  int count = 0;
  File root = SD.open("/");
  if (root) {
    while (count < 64) {
      File entry = root.openNextFile();
      if (!entry) break;
      String raw = String(entry.name());
      int sl = raw.lastIndexOf('/');
      String name = (sl >= 0) ? raw.substring(sl + 1) : raw;
      if (!entry.isDirectory() && name.startsWith("data_") && name.endsWith(".csv"))
        toDelete[count++] = "/" + name;
      entry.close();
    }
    root.close();
  }
  bool truncated = (count == 64);
  int deleted = 0;
  for (int i = 0; i < count; i++) {
    if (SD.remove(toDelete[i])) deleted++;
  }
  logLastPath = "";  // force header re-creation on next write
  String msg = "Deleted " + String(deleted) + " file(s)";
  if (truncated) msg += " — WARNING: >64 files found, run again to delete remaining";
  Serial.println(msg);
  server.send(200, "text/plain", msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi lifecycle
// ─────────────────────────────────────────────────────────────────────────────
bool startWifi() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(WIFI_SSID, WIFI_PASS)) {
    Serial.println("WiFi AP failed to start");
    return false;
  }

  // Register routes once; server.stop() does not clear handlers
  static bool routesRegistered = false;
  if (!routesRegistered) {
    server.on("/",           HTTP_GET,  handleRoot);
    server.on("/download",   HTTP_GET,  handleDownload);
    server.on("/rtc-sync",   HTTP_POST, handleRTCSync);
    server.on("/exit",       HTTP_POST, handleExit);
    server.on("/delete",     HTTP_POST, handleDelete);
    server.on("/delete-all", HTTP_POST, handleDeleteAll);
    routesRegistered = true;
  }
  server.begin();
  wifiExitReq = false;
  Serial.println("WiFi AP started — connect to \"" WIFI_SSID "\" then open http://192.168.4.1");
  return true;
}

void stopWifi() {
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiExitReq = false;
  Serial.println("WiFi stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop()
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  checkButtonEvents();

  if (isWiFiMode) {
    server.handleClient();
    blinkLEDWifi();

    static unsigned long lastWifiSample = 0;
    if (millis() - lastWifiSample >= 3000) {
      lastWifiSample = millis();
      continuousSampling();
    }

    if (wifiExitReq) {
      stopWifi();
      isWiFiMode = false;
    }
    delay(10);
    return;
  }

  // Normal mode
  continuousSampling();
  for (int j = 0; j < 30; j++) {
    checkButtonEvents();
    checkDisplayTimeout();
    updateLED();
    delay(100);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
String getTimestamp(const DateTime &dt) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());
  return String(buf);
}

void displayOLED(const DateTime &dt, bool dtValid,
                 float busV, float currMa, float powerMw,
                 bool overflow) {
  char buf[22];
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (dtValid) {
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             dt.year(), dt.month(), dt.day(),
             dt.hour(), dt.minute(), dt.second());
  } else {
    snprintf(buf, sizeof(buf), "No RTC");
  }
  display.setCursor(0, 0);  display.print(buf);

  snprintf(buf, sizeof(buf), "Bus:  %6.3f V", busV);
  display.setCursor(0, 8);  display.print(buf);

  snprintf(buf, sizeof(buf), "Curr: %6.2f mA", currMa);
  display.setCursor(0, 16); display.print(buf);

  snprintf(buf, sizeof(buf), "Powr: %6.1f mW", powerMw);
  display.setCursor(0, 24); display.print(buf);

  if (!sdAvailable) {
    snprintf(buf, sizeof(buf), "SD: NO CARD");
  } else if (sdWriteOk) {
    snprintf(buf, sizeof(buf), "SD: OK");
  } else {
    snprintf(buf, sizeof(buf), "SD: WRITE FAIL");
  }
  display.setCursor(0, 32); display.print(buf);

  display.setCursor(0, 40);
  display.print(overflow ? "Status: OVERFLOW!" : "Status: OK");

  display.display();
}

void logToSD(const String &timestamp,
             float busV, float currMa, float powerMw,
             bool overflow) {
  if (!sdAvailable) return;

  // Build daily filename: /data_YYYY-MM-DD.csv (fallback: /data_no-rtc.csv)
  String dateStr = (timestamp.length() >= 10) ? timestamp.substring(0, 10) : "no-rtc";
  String path = "/data_" + dateStr + ".csv";

  // Write CSV header only when the file is first created (checked once per day)
  if (path != logLastPath) {
    if (!SD.exists(path)) {
      File fh = SD.open(path, FILE_WRITE);
      if (fh) {
        fh.println("datetime,bus_V,current_mA,power_mW,overflow");
        fh.close();
      } else {
        sdWriteOk = false;
        return;
      }
    }
    logLastPath = path;
  }

  File f = SD.open(path, FILE_APPEND);
  if (!f) {
    Serial.println("SD write error: " + path);
    sdWriteOk = false;
    return;
  }
  f.print(timestamp); f.print(",");
  f.print(busV, 3);    f.print(",");
  f.print(currMa, 2);  f.print(",");
  f.print(powerMw, 2); f.print(",");
  f.println(overflow ? 1 : 0);
  f.close();
  sdWriteOk = true;
}

void continuousSampling() {
  if (!isWiFiMode) updateLED();
  if (!inaAvailable) return;

  ina226.readAndClearFlags();
  float busV     = ina226.getBusVoltage_V();
  float currMa   = ina226.getCurrent_mA();
  float powerMw  = ina226.getBusPower();
  bool  overflow = ina226.overflow;

  DateTime now((uint32_t)0);
  String ts;
  if (rtcAvailable) {
    now = rtc.now();
    ts  = getTimestamp(now);
  } else {
    ts = "NO-RTC";
  }

  // Save for web page
  lastBusV    = busV;
  lastCurrMa  = currMa;
  lastPowerMw = powerMw;
  lastOverflow = overflow;
  lastTs      = ts;

  Serial.println("=== " + ts + " ===");
  Serial.print("Bus Voltage [V]:  "); Serial.println(busV, 3);
  Serial.print("Current     [mA]: "); Serial.println(currMa, 2);
  Serial.print("Power       [mW]: "); Serial.println(powerMw, 2);
  Serial.println(overflow ? "Status: OVERFLOW!" : "Status: OK");
  Serial.println();

  checkButtonEvents();
  checkDisplayTimeout();
  if (displayAvailable && displayWakeMs > 0) {
    displayOLED(now, rtcAvailable, busV, currMa, powerMw, overflow);
  }
  logToSD(ts, busV, currMa, powerMw, overflow);
}
