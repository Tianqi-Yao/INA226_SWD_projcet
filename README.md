# 电池电压电流监测仪

基于 **ESP32-C3 Super Mini** 的便携式电池监测系统，通过 INA226 传感器实时采集电压、电流、功率等数据，显示在 0.96 寸 OLED 屏幕上，并由 DS3231 RTC 打上时间戳后写入 MicroSD 卡留存。

---

## 功能特性

- 实时测量总线电压、负载电压、分流电压、电流、功率
- 估算电池电量百分比
- 0.96 寸 OLED（128×64）满屏 8 行信息展示
- DS3231 RTC 提供精准时间戳，断电后时间不丢失
- 每次采样自动追加写入 SD 卡 CSV 文件，便于后续分析
- INA226 采样 5 次后自动进入省电模式 7 秒，循环运行
- 串口同步输出所有测量值，方便调试

---

## 硬件清单

| 器件 | 型号 / 规格 |
|------|------------|
| 微控制器 | ESP32-C3 Super Mini |
| 电流/电压传感器 | INA226（最高支持 36V） |
| 显示屏 | 0.96 寸 OLED，128×64，I2C（SSD1306） |
| 实时时钟 | DS3231 RTC 模块 |
| 存储模块 | MicroSD Card Module（SPI 接口） |

---

## 接线说明

### I2C 总线（OLED + INA226 + DS3231 共享）

| 设备引脚 | ESP32-C3 Super Mini |
|---------|---------------------|
| SDA | GPIO 4 |
| SCL | GPIO 5 |
| VCC | 3.3V |
| GND | GND |

三个 I2C 设备通过不同地址区分：OLED `0x3C`、INA226 `0x40`、DS3231 `0x68`。

### SPI 总线（MicroSD）

| 设备引脚 | ESP32-C3 Super Mini |
|---------|---------------------|
| MOSI | GPIO 6 |
| MISO | GPIO 7 |
| SCK | GPIO 10 |
| CS | GPIO 3 |
| VCC | 3.3V |
| GND | GND |

### INA226 被测回路接线

| INA226 引脚 | 连接 |
|------------|------|
| IN+ | 被测电源正极 |
| IN- | 被测电源负极 / 负载正极 |

---

## 软件环境

### Arduino IDE 设置

1. 在 Boards Manager 搜索 `esp32`，安装 **esp32 by Espressif Systems**
2. 开发板选择：**ESP32C3 Dev Module**
3. **USB CDC On Boot** 设为 **Enabled**（让 Serial 映射到 USB 口）

### 需要安装的库

在 Sketch → Include Library → Manage Libraries 中搜索安装：

| 库名 | 作者 |
|------|------|
| Adafruit SSD1306 | Adafruit |
| Adafruit GFX Library | Adafruit |
| RTClib | Adafruit |
| INA226_WE | Wolfgang Ewald |

`Wire`、`SPI`、`SD` 为 Arduino ESP32 核心内置库，无需单独安装。

---

## 烧录与运行

1. 用 USB-C 连接 ESP32-C3 Super Mini
2. 在 Arduino IDE 中打开 `Voltage.ino`
3. 按上述设置选择开发板和端口后点击上传
4. 打开串口监视器（波特率 `115200`）查看初始化日志和实时数据

---

## OLED 显示布局

上电后 OLED 实时滚动显示 8 行信息：

```
2026-06-21 12:30:45
Bus:   12.345 V
Load:  12.346 V
Shnt:   1.234 mV
Curr:  456.78 mA
Powr: 5678.1 mW
Bat:    87.3 %
Status: OK
```

---

## SD 卡数据格式

SD 卡根目录下自动生成 `data.csv`，每次采样追加一行，重启后继续写入不覆盖：

```csv
datetime,bus_V,load_V,shunt_mV,current_mA,power_mW,battery_pct,overflow
2026-06-21 12:30:45,12.345,12.346,1.234,456.78,5678.90,87.3,0
```

`overflow` 字段为 `1` 表示电流超出 INA226 量程，需更换更大分流电阻或调整量程配置。

---

## 采样节奏

```
连续采样 5 次（每次间隔 3s）
         ↓
INA226 进入省电模式 7s
         ↓
INA226 唤醒，重新开始
```
