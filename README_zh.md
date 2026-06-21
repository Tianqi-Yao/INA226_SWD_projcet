# SWD INA226 电流电压监测仪

基于 **ESP32-C3 Super Mini** 的便携式电源监测系统，通过 INA226 传感器实时采集总线电压、电流、功率，由 DS3231 RTC 打上时间戳后写入 MicroSD 卡。支持 OLED 按键唤醒显示和 WiFi 热点远程查看。

---

## 功能特性

- 实时测量总线电压、电流、功率，每 3 秒采样一次持续循环
- DS3231 RTC 提供精准时间戳，断电后时间不丢失
- 每次采样自动追加写入 SD 卡 CSV 文件
- 短按 BOOT 键唤醒 OLED，15 秒无操作自动息屏
- **长按 BOOT 键 3 秒**启动 WiFi 热点，手机连接后访问 `192.168.4.1` 可：
  - 查看实时测量数据
  - 下载 `data.csv`
  - 用手机时间同步 RTC
- 板载 LED 状态指示：SD 异常或 INA226 故障时闪烁，正常时熄灭
- 串口同步输出所有测量值，方便调试
- 各外设独立降级：任一模块初始化失败不影响其他功能

---

## 硬件清单

| 器件 | 型号 / 规格 |
|------|------------|
| 微控制器 | ESP32-C3 Super Mini |
| 电流/电压传感器 | INA226（最高支持 36V，分流电阻 R100 = 100 mΩ） |
| 显示屏 | 0.96 寸 OLED，128×64，I2C（SSD1306）— 可选，移除后代码自动降级 |
| 实时时钟 | DS3231 RTC 模块 |
| 存储模块 | MicroSD Card Module（SPI 接口） |

---

## 接线说明

### I2C 总线（OLED + INA226 + DS3231 共享）

| 设备引脚 | ESP32-C3 Super Mini |
|---------|---------------------|
| SDA | GPIO 5 |
| SCL | GPIO 6 |
| VCC | 3.3V |
| GND | GND |

三个 I2C 设备通过不同地址区分：OLED `0x3C`、INA226 `0x40`、DS3231 `0x68`。

### SPI 总线（MicroSD）

GPIO1-4 连续排列，可并排焊接：

| 设备引脚 | ESP32-C3 Super Mini |
|---------|---------------------|
| MISO | GPIO 1 |
| CLK  | GPIO 2 |
| MOSI | GPIO 3 |
| CS   | GPIO 4 |
| VCC | 3.3V |
| GND | GND |

> ⚠️ GPIO2 是 ESP32-C3 的 strapping 引脚，启动时若被拉低会进入 USB-JTAG 模式。建议在 PCB 上对 GPIO2 加 10kΩ 上拉电阻至 3.3V。

### INA226 被测回路接线（高边采样）

```
电源正极 → IN+ ──[R100 分流]──→ IN− → 负载 → 电源负极（GND）
                                  ↑
                                 VBUS ← 接此节点
                                 GND  ← 接 ESP32 GND = 电源负极
```

| INA226 引脚 | 连接 |
|------------|------|
| IN+ | 电源正极（分流电阻前端） |
| IN- | 负载正端（分流电阻后端） |
| VBUS | 与 IN- 同节点 |
| GND | 与 ESP32 GND、电源负极共地 |

> VBUS 未接或 GND 不共地时，总线电压读数为 0。

### 板载引脚

| 功能 | GPIO |
|------|------|
| BOOT 按键 | GPIO 9（内部上拉，按下为 LOW） |
| 状态 LED | GPIO 8（低电平点亮） |

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

`Wire`、`SPI`、`SD`、`WiFi`、`WebServer` 为 Arduino ESP32 核心内置库，无需单独安装。

---

## 烧录与运行

1. 用 USB-C 连接 ESP32-C3 Super Mini
2. 在 Arduino IDE 中打开 `swd_ina226.ino`
3. 按上述设置选择开发板和端口后点击上传
4. 打开串口监视器（波特率 `115200`）查看初始化日志和实时数据

---

## OLED 显示布局

上电后显示 2 秒初始化画面，随后自动息屏。**短按 BOOT 键**唤醒，15 秒无操作后再次息屏：

```
2026-06-21 12:30:45
Bus:   5.091 V
Curr: 391.42 mA
Powr: 1990.3 mW
SD: OK
Status: OK
```

| 行 | 内容 |
|----|------|
| 第1行 | RTC 时间（无 RTC 时显示 `No RTC`） |
| 第2行 | 总线电压（IN− 至 GND） |
| 第3行 | 电流 |
| 第4行 | 功率 |
| 第5行 | SD 卡状态：`OK` / `WRITE FAIL` / `NO CARD` |
| 第6行 | 测量状态：`OK` / `OVERFLOW!` |

---

## WiFi 热点

**长按 BOOT 键 3 秒**启动 WiFi AP：

| 项目 | 值 |
|------|----|
| SSID | `SWD_INA226` |
| 密码 | `12345678` |
| 地址 | `http://192.168.4.1` |

Web 页面提供：

- 实时数据（每次刷新读取最新采样）
- **Download CSV** — 直接下载 `data.csv`
- **Sync RTC with Phone** — 用手机当前时间同步 DS3231
- **Exit WiFi** — 关闭热点，恢复正常模式

WiFi 模式期间每 3 秒继续采样，LED 持续闪烁提示热点活跃。

---

## SD 卡数据格式

SD 卡根目录下自动生成 `data.csv`，每次采样追加一行，重启后继续写入不覆盖：

```csv
datetime,bus_V,current_mA,power_mW,overflow
2026-06-21 12:30:45,5.091,391.42,1990.30,0
```

| 字段 | 说明 |
|------|------|
| `datetime` | RTC 时间戳，无 RTC 时为 `NO-RTC` |
| `bus_V` | IN− 引脚对 GND 的电压（负载侧电压） |
| `current_mA` | 电流，由分流电压 ÷ 100 mΩ 计算 |
| `power_mW` | 功率（INA226 内部寄存器计算） |
| `overflow` | `1` 表示电流超出量程（> 819 mA），数值不可信 |

---

## LED 状态

| LED 状态 | 含义 |
|---------|------|
| 常灭 | 正常运行，SD 和 INA226 均正常 |
| 1Hz 闪烁 | SD 未挂载 / 写入失败 / INA226 故障 |
| 1Hz 闪烁（WiFi 模式） | WiFi 热点活跃 |

---

## 采样节奏

```
INA226 采样 → 写入 SD → 等待 3 秒 → 循环
```
