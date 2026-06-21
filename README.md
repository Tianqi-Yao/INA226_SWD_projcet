# SWD INA226 Power Monitor

A portable power monitoring system based on the **ESP32-C3 Super Mini**, using an INA226 sensor to measure bus voltage, current, and power in real time. Readings are timestamped by a DS3231 RTC and logged to a MicroSD card. Supports OLED display with button wake and a WiFi hotspot for remote access.

---

## Features

- Real-time measurement of bus voltage, current, and power — sampled every 3 seconds
- DS3231 RTC timestamps every reading; time is retained across power cycles
- Readings appended to a CSV file on the SD card
- Short-press BOOT button to wake the OLED; auto-off after 15 seconds of inactivity
- **Long-press BOOT button (3 s)** to start a WiFi access point — connect your phone and open `192.168.4.1` to:
  - View live measurements
  - Download `data.csv`
  - Sync the RTC from your phone's clock
- Onboard LED status indicator: blinks on SD or INA226 fault, off when all is well
- Serial output of all values for debugging
- Graceful degradation: each peripheral initializes independently; a missing module does not affect the rest

---

## Hardware

| Component | Part |
|-----------|------|
| Microcontroller | ESP32-C3 Super Mini |
| Current/voltage sensor | INA226 (up to 36 V, onboard shunt R100 = 100 mΩ) |
| Display | 0.96" OLED 128×64 I2C (SSD1306) — optional; removing it requires no code change |
| Real-time clock | DS3231 RTC module |
| Storage | MicroSD card module (SPI) |

---

## Wiring

### I2C bus — OLED, INA226, DS3231 (shared)

| Device pin | ESP32-C3 Super Mini |
|-----------|---------------------|
| SDA | GPIO 5 |
| SCL | GPIO 6 |
| VCC | 3.3 V |
| GND | GND |

The three I2C devices are distinguished by address: OLED `0x3C`, INA226 `0x40`, DS3231 `0x68`.

### SPI bus — MicroSD

GPIOs 1–4 are consecutive for easy soldering:

| Device pin | ESP32-C3 Super Mini |
|-----------|---------------------|
| MISO | GPIO 1 |
| CLK  | GPIO 2 |
| MOSI | GPIO 3 |
| CS   | GPIO 4 |
| VCC | 3.3 V |
| GND | GND |

> ⚠️ GPIO2 is a strapping pin on the ESP32-C3. If it is pulled low at boot, the chip enters USB-JTAG download mode. Add a 10 kΩ pull-up resistor from GPIO2 to 3.3 V on your PCB.

### INA226 — high-side current sensing

```
Supply+ → IN+ ──[R100 shunt]──→ IN− → Load → Supply−
                                  ↑
                                VBUS ← connect here
                                GND  ← connect to ESP32 GND = Supply−
```

| INA226 pin | Connection |
|-----------|------------|
| IN+ | Supply positive (before shunt) |
| IN− | Load positive (after shunt) |
| VBUS | Same node as IN− |
| GND | Common ground with ESP32 and supply negative |

> Bus voltage reads 0 if VBUS is left unconnected or GND is not shared.

### Onboard pins

| Function | GPIO |
|----------|------|
| BOOT button | GPIO 9 (internal pull-up, LOW when pressed) |
| Status LED | GPIO 8 (active LOW) |

---

## Software Setup

### Arduino IDE

1. In Boards Manager, search for `esp32` and install **esp32 by Espressif Systems**
2. Select board: **ESP32C3 Dev Module**
3. Set **USB CDC On Boot** to **Enabled** (maps `Serial` to the USB port)

### Libraries to install

Search and install via Sketch → Include Library → Manage Libraries:

| Library | Author |
|---------|--------|
| Adafruit SSD1306 | Adafruit |
| Adafruit GFX Library | Adafruit |
| RTClib | Adafruit |
| INA226_WE | Wolfgang Ewald |

`Wire`, `SPI`, `SD`, `WiFi`, and `WebServer` are bundled with the ESP32 Arduino core — no separate installation needed.

---

## Flashing

1. Connect the ESP32-C3 Super Mini via USB-C
2. Open `swd_ina226.ino` in Arduino IDE
3. Select the correct board and port, then click Upload
4. Open Serial Monitor at **115200 baud** to view the init log and live readings

---

## OLED Display

The OLED shows a 2-second splash screen on boot, then turns off. **Short-press the BOOT button** to wake it; it turns off again after 15 seconds of inactivity.

```
2026-06-21 12:30:45
Bus:   5.091 V
Curr: 391.42 mA
Powr: 1990.3 mW
SD: OK
Status: OK
```

| Row | Content |
|-----|---------|
| 1 | RTC timestamp (shows `No RTC` if unavailable) |
| 2 | Bus voltage (IN− to GND) |
| 3 | Current |
| 4 | Power |
| 5 | SD card status: `OK` / `WRITE FAIL` / `NO CARD` |
| 6 | Measurement status: `OK` / `OVERFLOW!` |

---

## WiFi Hotspot

**Long-press the BOOT button for 3 seconds** to start the access point:

| | |
|-|-|
| SSID | `SWD_INA226` |
| Password | `12345678` |
| URL | `http://192.168.4.1` |

The web page provides:

- Live data (refreshed on each page load)
- **Download CSV** — downloads `data.csv` directly to your device
- **Sync RTC with Phone** — sets the DS3231 to your phone's current time
- **Exit WiFi** — shuts down the hotspot and returns to normal mode

Sampling continues every 3 seconds while the hotspot is active. The LED blinks continuously to indicate WiFi is on.

---

## CSV Format

`data.csv` is created automatically in the SD card root directory. Each sample appends one row; existing data is never overwritten on reboot.

```csv
datetime,bus_V,current_mA,power_mW,overflow
2026-06-21 12:30:45,5.091,391.42,1990.30,0
```

| Field | Description |
|-------|-------------|
| `datetime` | RTC timestamp; `NO-RTC` if the clock is unavailable |
| `bus_V` | Voltage at IN− relative to GND (load-side voltage) |
| `current_mA` | Current derived from shunt voltage ÷ 100 mΩ |
| `power_mW` | Power from INA226 internal register |
| `overflow` | `1` when current exceeds the measurement range (> 819 mA); values are unreliable |

---

## LED Status

| LED | Meaning |
|-----|---------|
| Off | Normal operation; SD and INA226 both healthy |
| Blinking 1 Hz | SD not mounted, write failure, or INA226 fault |
| Blinking 1 Hz (WiFi mode) | WiFi hotspot active |

---

## Sampling Cycle

```
Read INA226 → Write SD → Wait 3 s → repeat
```
