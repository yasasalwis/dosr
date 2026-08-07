# Bill of Materials — Mini Feeder

Four-channel 12 V dosing controller. Quantities are for one complete build.

Every line carries a **Firmware ref** so it can be checked against the code
rather than taken on trust — pin names come from `include/config.h`, addresses
and ranges from the drivers that use them.

> **Decided:** the pump gates and the float switch move onto an **MCP23017**
> I²C expander, freeing four MCU pins for the SPI display. Section 2 shows the
> arithmetic. The firmware has been updated to match — `pumps.cpp` drives the
> expander, `ui.cpp` drives the ST7735.

---

## 1. Confirmed parts

| # | Part | Qty | Firmware ref | Status |
|---|---|---|---|---|
| 1.1 | **Seeed Studio XIAO ESP32-C5** | 1 | `board = seeed_xiao_esp32c5` | ✅ matches firmware |
| 1.2 | **pH sensor module** + BNC electrode probe, pH 0–14 | 1 | `ADS_CH_PH = 0` | ✅ matches firmware |
| 1.3 | **Peristaltic dosing pump** — select the **12 V** variant | 4 | `MCP_PUMP_PINS[4]` | ✅ matches firmware |
| 1.4 | **TDS sensor module**, analog out, bundled **DS18B20** temp probe | 1 | `ADS_CH_EC = 1`, `PIN_DS18B20` | ✅ both halves driven |
| 1.5 | **1.8″ TFT + EC11 rotary encoder combo module**, SPI | 1 | `PIN_TFT_*` | ✅ `ui.cpp` now drives ST7735 |

The pump listing also sells 3 V, 6 V and 24 V — pick 12 V to match the PSU and
the MOSFET drive. If it offers a tubing choice, BPT outlasts silicone under
peristaltic squeeze; throughput dropping as the tube flattens is what forces
recalibration of `cfg.pump[i].mlPerSec`.

---

## 2. Pin budget — the blocking constraint

The XIAO ESP32-C5 breaks out **11 GPIOs** (D0–D10). The variant also defines
`A1`–`A4` (GPIO 2,3,4,5), `BAT_VOLT_PIN` (GPIO6) and `LED_BUILTIN` (GPIO27),
but none of those reach the edge pads — they are not wirable. `A0` is the only
exposed ADC pin, and it is GPIO1 = D0, which the encoder switch already uses.
That is why the design needs an ADS1115 at all.

What the confirmed parts require:

| Function | Pins | Notes |
|---|---:|---|
| EC11 encoder (A, B, switch) | 3 | from the combo module |
| Pump gates ×4 | 4 | one per MOSFET |
| I²C — ADS1115 | 2 | still required; only one ADC pad exists |
| SPI TFT (SCK, MOSI, DC, CS) | 4 | RST → 3V3, BLK → 3V3, MISO unused |
| DS18B20 temperature | 1 | bundled with the TDS module |
| Float switch | 1 | optional |
| **Total** | **15** | **against 11 available** |

Stripped to the absolute minimum — no float switch, no DS18B20, CS tied to GND
so the TFT takes 3 pins — it is still **12 against 11**. It does not fit.

There is a second collision worth knowing about: the board's hardware-SPI pins
are SCK=GPIO8 (**D8**), MISO=GPIO9 (**D9**), MOSI=GPIO10 (**D10**), SS=GPIO7
(**D3**) — which is pumps 3, 4, 1 and the float switch. The ESP32-C5's GPIO
matrix can route SPI to any pin, so this is not fatal, but nothing is free to
route it *to*.

### Chosen: I²C GPIO expander for the pump gates

The four MOSFET gates and the float switch move onto an **MCP23017**, freeing
D3, D7, D8, D9 and D10.

| Function | Pins |
|---|---:|
| Encoder 3 · I²C 2 · TFT 4 · DS18B20 1 · UART0 TX 1 | **11** |

Fits exactly, keeps the TFT, and keeps the D6 debug line. Final map:

| Pad | GPIO | Function |
|---|---|---|
| D0 | 1 | Encoder switch |
| D1 | 0 | Encoder A |
| D2 | 25 | Encoder B |
| D3 | 7 | TFT CS *(native SPI SS)* |
| D4 | 23 | I²C SDA |
| D5 | 24 | I²C SCL |
| D6 | 11 | *(free — UART0 TX)* |
| D7 | 12 | TFT DC |
| D8 | 8 | TFT SCK *(native)* |
| D9 | 9 | DS18B20 *(reserved)* |
| D10 | 10 | TFT MOSI *(native)* |

MCP23017 `0x20`: GPA0–GPA3 = pump gates, GPB0 = float switch.

D9 stays free because `SPI.begin()` is called with `miso = -1` — the core
skips `spiAttachMISO()` for a negative pin, so the peripheral never claims
GPIO9. TFT `RST` and `BLK` tie to 3V3.

**Use an MCP23017, not a PCF8574.** The MCP23017 powers up with every pin as a
high-Z input, so the 100 kΩ gate pull-downs hold the pumps off through reset —
the existing safety property survives. A PCF8574's quasi-bidirectional outputs
power up **high**, which would turn all four pumps on at power-up.

The honest cost: `pumps.stopAll()` stops being a direct GPIO write and becomes
an I²C transaction, so a wedged bus means you cannot stop a running pump. The
firmware handles this explicitly rather than leaving it to the watchdog — every
gate write is read back, and if a *stop* keeps failing for 2 s it resets the
board on purpose, which returns the expander to high-Z and lets the pull-downs
cut the pumps. Bounded, but a failure window that did not exist before.

### Rejected: keep the I²C OLED

A 128×64 SSD1306 plus the encoder half of the combo module also fits exactly
and needs no display firmware work, but shelves the TFT. Recorded here only so
the trade-off is not re-litigated later.

---

## 3. Pump drive — ×4 channels

| Part | Qty | Spec | Why |
|---|---|---|---|
| Logic-level N-channel MOSFET | 4 | **AO3400** or **IRLB8721** | A plain IRF540N will not fully turn on from 3.3 V |
| Gate pull-down resistor | 4 | **100 kΩ**, gate → GND | **Do not omit.** Holds gates low through reset and boot, before any code runs. Without it a pump can run a few hundred ms at power-up — a real uncommanded dose |
| Gate series resistor | 4 | **100 Ω**, driver → gate | Limits gate-charge current spikes |
| Flyback diode | 4 | **1N5822** Schottky or 1N4007, cathode to +12 V | Clamps the motor's inductive kick |
| MCP23017 I²C expander | 1 | **Required** | See section 2. Not a PCF8574 — its outputs wake high and would start every pump at power-up |

A ready-made 4-channel logic-level MOSFET module replaces the first four rows —
but verify it actually has gate pull-downs and genuinely logic-level FETs.

---

## 4. Power

| Part | Qty | Spec | Note |
|---|---|---|---|
| PSU, 12 V DC | 1 | **3 A** min | Firmware runs one pump at a time — requests queue — so 3 A covers one motor plus margin |
| Buck converter | 1 | 12 V → **5 V**, ≥1 A, into the XIAO `5V` pad | — |
| Electrolytic capacitor | 1 | **470–1000 µF**, ≥25 V, near the FETs | Absorbs motor inrush |

**Never run USB and the buck converter into the 5V pin together.** Every boot
prints its reset reason; `BROWNOUT` is the symptom of exactly that mistake.
Star the grounds at the PSU — pump return current sharing a path with the
sensor board lands on the pH reading as noise.

---

## 5. Analog front end

| Part | Qty | Spec | Firmware ref |
|---|---|---|---|
| ADS1115 breakout | 1 | 16-bit I²C, address **0x48** (ADDR→GND), run at **3.3 V** | `ADDR_ADS1115`, `GAIN_ONE`, `RATE_ADS1115_128SPS` |
| Divider resistors | 4 | **10 kΩ**, two per analog channel | `ADC_DIVIDER_RATIO = 2.0f` |
| I²C pull-ups | 0–2 | 4.7 kΩ to 3.3 V, only if your breakouts lack them | `I2C_CLOCK_HZ` = 100 kHz |
| DS18B20 pull-up | 1 | **4.7 kΩ from D9 to 3V3** — required, the probe is open-drain | `PIN_DS18B20` |

Both sensor boards run at 5 V; the ADS1115 runs at 3.3 V, so each output goes
through a 2:1 divider into A0 / A1:

```
sensor out ──[10k]──┬──[10k]── GND
                    └── ADS1115 A0 / A1
```

`ADC_DIVIDER_RATIO` multiplies readings back so calibration figures stay in the
sensor's own 0–5 V domain. Note it is a **single global constant applied to
both channels** — you cannot drop the divider on just one channel without a
code change. A typical TDS module swings only 0–2.3 V, so after halving it uses
about a quarter of the ADC span; still ~9,000 counts at `GAIN_ONE`, which is
ample for EC.

Bus speed is deliberately 100 kHz. At 400 kHz down jumper wires the rise times
don't make it and the bus reports `ESP_ERR_INVALID_STATE` rather than failing
cleanly. **No level shifter** — every I²C device here is a 3.3 V part.

---

## 6. Firmware implications of parts 1.4 and 1.5

**TDS module — analog side needs no changes.** A TDS probe *is* a conductivity
probe; "TDS in ppm" is just scaled EC. Calibrate `ecCalFactor` against a
1413 µS/cm standard and `ecFromVolts()` reads µS/cm directly, exactly as
written. Nothing to change.

**DS18B20 — implemented.** The bundled probe drives the 2 %/°C compensation in
`ecFromVolts()` automatically. `cfg.tempC` survives as the fallback, relabelled
*Manual C* in the menu, and is what gets used when no probe answers — unplugging
the probe degrades to the old behaviour rather than failing. Conversions are
asynchronous so nothing blocks `loop()`. Needs a **4.7 kΩ pull-up from D9 to
3V3**, and adds a temperature entity to Home Assistant discovery.

It adds **no libraries**: OneWire does not compile for the ESP32-C5, because
its ESP32 path writes the GPIO registers directly and IDF 5.5 makes those
bitfield unions rather than `uint32_t`. `src/ds18b20.cpp` drives the probe
through the Arduino API instead.

**1.8″ TFT — `ui.cpp` rewritten.** It is an SPI ST7735 (128×160, 16-bit
colour) where the firmware drove an I²C SSD1306 (128×64, mono). What changed:

- **Framebuffer.** `Adafruit_ST7735` draws straight to the panel, so the
  `clearDisplay() → draw → display()` pattern the menu is built around would
  tear at a 10 Hz refresh. Drawing now goes into a `GFXcanvas16` pushed in one
  blit — 128 × 160 × 2 = 40 KB against 320 KB of SRAM.
- **No presence detection.** `i2cProbe()` worked because I²C acknowledges. SPI
  does not, so `ui.begin()` reports what it configured, not what replied, and
  the "running headless" path is gone.
- **Layout.** Ten menu rows instead of five, and the home screen gained
  deadband bands under each reading — the same idea as the web panel.
- **Colour.** The palette is lifted from `webpage.h` so the display and the web
  page read as one product.

`lib_deps` gains `adafruit/Adafruit ST7735 and ST7789 Library` and
`adafruit/Adafruit MCP23017 Arduino Library`; `Adafruit SSD1306` is dropped.

---

## 7. Plumbing and consumables

| Part | Qty | Spec |
|---|---|---|
| Silicone tubing | ~2 m | to match pump head bore |
| Dosing bottles / reservoirs | 4 | one per nutrient |
| pH 7.00 buffer | 1 | two-point calibration, first point |
| pH 4.00 buffer | 1 | two-point calibration, second point |
| EC standard | 1 | **1413 µS/cm** — the firmware's default `ecCalStandard` |
| Measuring cylinder | 1 | 25–50 ml, for pump calibration (*Run 20s* → measure → enter) |
| Reservoir float switch | 0–1 | optional, `cfg.floatEnabled`, `cfg.floatActiveLow` |
| USB-TTL serial adapter | 0–1 | 3.3 V, to **D6 (TX) only**, VCC disconnected |

D7 is Pump 2's gate, so there is no UART0 RX — the debug port is listen-only.
Arduino `Serial` goes over USB-C by default (`ARDUINO_USB_CDC_ON_BOOT=1`).

---

## 8. Enclosure / 3D printed parts

> **Not yet available.** No STL, STEP, 3MF or other CAD file exists in this
> repository — not on `main`, not on the feature branch, not anywhere in git
> history.

| Part | Qty | Material | File |
|---|---|---|---|
| _TBD_ | | | |

`docs/models/` alongside this file is the natural home, with print settings
(layer height, infill, supports, material) recorded per part.

---

## 9. Fasteners and wiring

Driven by the enclosure design, so this fills in with section 8. Expect:

- M3 screws and heat-set inserts
- JST-XH or screw terminals for pump and sensor connections
- Silicone hook-up wire — 20 AWG for 12 V pump runs, 26 AWG for signals
- Panel-mount barrel jack for the 12 V input
- Knob for the rotary encoder
