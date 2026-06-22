# Effects of Shunt Resistor, Extra Load, and Long Cables on Measurements

An analysis grounded in real measured data.

---

## 1. Our Actual Measurements

System: INA226 + R100 shunt resistor (100 mΩ)

| Source | Voltage | Current | Power |
|--------|---------|---------|-------|
| INA226 | 5.091 V (Bus, IN− side) | 391.42 mA | 1990.3 mW |
| External meter | 5.131 V | 389 mA | 2.082 W |

The INA226 Bus Voltage measures IN− relative to GND (load side). The supply-side voltage (IN+) can be reconstructed:

```
V_IN+ = V_IN- + I × R_shunt
      = 5.091 + 0.391 × 0.1
      = 5.091 + 0.039
      = 5.130 V
```

**This matches the external meter's 5.131 V to within 1 mV, confirming that high-side sensing and shunt reconstruction are working correctly.**

The small power discrepancy (INA226: 1.990 W, meter: 2.082 W) comes from:
- INA226 computes power as `V_bus × I` — load-side voltage times current, excluding the shunt's own dissipation
- The external meter is measuring at a different point in the circuit and may include additional cable drop

---

## 2. The Shunt Resistor's Own Impact

R100 = 0.1 Ω is a series impedance inserted into the measurement loop. Its effect grows with current.

### Voltage drop

```
V_drop = I × R_shunt = I × 0.1
```

| Current | R100 drop | Load voltage received (assuming 5.00 V supply) |
|---------|-----------|------------------------------------------------|
| 100 mA | 10 mV | 4.990 V |
| 391 mA | 39 mV | 4.961 V |
| 500 mA | 50 mV | 4.950 V |
| 819 mA (range limit) | 82 mV | 4.918 V |

**With a fixed supply voltage, a heavier load on the IN− side means more current, a larger drop across R100, and a lower V_IN−.** The load voltage decreases monotonically with current — this is not an INA226 issue but an inherent consequence of a series shunt resistor. For USB-powered devices (minimum 4.75 V) or Raspberry Pis (sensitive to undervoltage), this steadily eroding margin becomes more critical as current grows.

### Shunt self-dissipation

```
P_shunt = I² × R = 0.391² × 0.1 ≈ 15.3 mW
```

At the 819 mA range limit:

```
P_shunt = 0.819² × 0.1 ≈ 67 mW
```

The dissipation is modest, but sustained high current causes the resistor to heat up, shifting its resistance value (resistors have a non-zero temperature coefficient) and introducing a small measurement drift.

---

## 3. The Monitor Itself as an Extra Load

This device (ESP32 + INA226 + OLED + RTC + SD card) draws roughly 30–80 mA of its own (more with WiFi active). Whether that current pollutes the measurement depends entirely on **where the device is powered from**.

### Powered from IN+ (recommended)

```
Supply+ ─┬─→ IN+ ─[R100]─→ IN− ─→ Load under test ─→ GND
          └──────→ Monitor supply ────────────────────→ GND
```

The monitor's current bypasses R100 entirely. Measured current = load current only. **Monitor self-consumption has zero effect on the reading.**

### Powered from IN−

```
Supply+ ──→ IN+ ─[R100]─→ IN− ─┬─→ Load under test ─→ GND
                                 └──→ Monitor supply ──→ GND
```

The monitor's current flows through R100. Measured current = load + monitor. **Readings are inflated by 30–80 mA.**

---

## 4. Long Cables and Their Effect on Measurements

Wire has resistance. Longer and thinner cables mean more voltage drop and more error in the end-to-end voltage budget.

### Copper wire resistance reference (one way)

| Gauge | Resistance (Ω/m) | 1 m round trip (Ω) |
|-------|-----------------|---------------------|
| 20 AWG | 0.033 | 0.067 |
| 22 AWG | 0.053 | 0.106 |
| 24 AWG | 0.084 | 0.168 |
| 26 AWG | 0.134 | 0.268 |
| 28 AWG | 0.213 | 0.426 |

### At our actual current (391 mA)

| Gauge | 1 m round-trip R | Extra voltage drop | Multiple of R100 |
|-------|------------------|--------------------|-----------------|
| 20 AWG | 0.067 Ω | 26 mV | 0.67× |
| 24 AWG | 0.168 Ω | 66 mV | 1.68× |
| 26 AWG | 0.268 Ω | 105 mV | 2.68× |
| 28 AWG | 0.426 Ω | 167 mV | 4.26× |

**Key takeaway: 1 m of 26 AWG cable drops 2.68× more voltage than R100 itself.**

### Where does cable drop sit in the circuit?

Cable resistance is distributed throughout the loop:

```
Supply+ ──[R_wire+]──→ IN+ ─[R100]─→ IN− ──[R_wire-]──→ Load ──→ GND
```

The INA226 only sees the differential voltage between IN+ and IN− (the shunt drop). **Cable resistance is invisible to it.**

Consequences:
- Longer cables → lower Bus Voltage (load receives less)
- Measured current is still the true current (series current is the same everywhere)
- Measured power = `V_bus × I` = power actually consumed by the load, **excluding cable losses**

To capture total power including cable dissipation, the measurement point must move to the very start of the cable.

---

## 5. Shunt Resistor Selection

| Shunt | Range limit (INA226 ±81.92 mV) | Drop at limit | Typical use |
|-------|-------------------------------|---------------|-------------|
| R100 (100 mΩ) | 819 mA | 82 mV | Low-power loads, < 500 mA |
| R050 (50 mΩ) | 1.64 A | 82 mV | Medium loads, Raspberry Pi Zero |
| R010 (10 mΩ) | 8.19 A | 82 mV | High-current devices, RPi 4B, charger testing |
| R005 (5 mΩ) | 16.4 A | 82 mV | High-power applications |

**The pattern:** a smaller shunt means a wider range and less voltage drop, but lower resolution (the same shunt voltage now represents a larger current step).

### Our R100 — when it fits and when it doesn't

- ✅ Loads below ~600 mA (small battery-powered MCU systems)
- ✅ When high resolution matters (~2.5 µA/LSB)
- ⚠️ Raspberry Pi 3B/4B (typically 500–1500 mA): range is too narrow, overflow likely
- ❌ Any load above 819 mA: triggers OVERFLOW, all readings invalid

---

## 6. Summary

Starting from 391 mA @ 5.091 V:

1. **R100 drops 39 mV** — 0.78% of a 5 V supply; acceptable for most loads
2. **Power the monitor from IN+** — its 50–80 mA self-consumption stays out of the measurement
3. **Cables over 1 m** — use 22 AWG or heavier; thinner wire can drop more than the shunt itself
4. **What the INA226 sees** — current through R100 and voltage at IN−; cable losses are outside its view
5. **Need more range** — swap to R010; no firmware changes required
