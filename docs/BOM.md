# Bill of Materials — Mini Feeder

Four-channel 12 V dosing controller. Quantities are for one complete build.

Every line carries a **Firmware ref** so it can be checked against the code
rather than taken on trust — pin names come from `include/config.h`, addresses
and ranges from the drivers that use them.

Prices are as they appeared in the source links (LKR, Sri Lanka storefront,
August 2026); the USD column uses the rate implied by those same links
(≈ LKR 376 / USD). AliExpress promo pricing moves constantly — treat these as
order-of-magnitude, not a quote.

---

## 1. Sourced parts

These are the four listings supplied for this build.

| # | Part | Qty | Firmware ref | LKR | ≈ USD | Source |
|---|---|---|---|---|---|---|
| 1.1 | ESP32-C5 development board — **needs confirmation, see note** | 1 | `board = seeed_xiao_esp32c5` | 3,034 | 8.15 | [1005010803715995](https://www.aliexpress.com/item/1005010803715995.html) |
| 1.2 | pH sensor module + BNC pH electrode probe, pH 0–14 | 1 | `ADS_CH_PH = 0`, `cfg.phVolt4/7` | 2,620 | 6.98 | [1005006366896348](https://www.aliexpress.com/item/1005006366896348.html) |
| 1.3 | **Unidentified — see note** | ? | — | 1,470 | 3.92 | [1005007511446508](https://www.aliexpress.com/item/1005007511446508.html) |
| 1.4 | Micro self-priming peristaltic dosing pump, **select the 12 V variant** | 4 | `PUMP_PINS[4]` | 1,353 ea | 3.60 ea | [1005004199838762](https://www.aliexpress.com/item/1005004199838762.html) |

**Note on 1.1** — the listing could not be read directly (AliExpress is
unreachable from this environment). It is cited by a January 2026 article
rounding up ESP32-C5 boards, so it is an ESP32-C5 of some kind, but *which*
one is unconfirmed. The firmware is built for the **Seeed XIAO ESP32-C5**
specifically: `platformio.ini` sets `board = seeed_xiao_esp32c5`, and the whole
pin map depends on that variant's `D0`–`D10` numbering
(D0=GPIO1, D1=GPIO0, D2=GPIO25, D3=GPIO7, D4=GPIO23, D5=GPIO24, D6=GPIO11,
D7=GPIO12, D8=GPIO8, D9=GPIO9, D10=GPIO10). A DevKitC or other C5 board will
compile only after changing `board =` and re-deriving every pin.

**Note on 1.3** — this listing returned nothing identifiable. At ~USD 3.92 it
is plausibly the OLED, the ADS1115, the encoder, the MOSFET module or the buck
converter — all of which appear below as separate lines. Confirm what it is
and delete whichever duplicate line it covers.

**Note on 1.4** — buy **four**, and pick the **12 V** option from the variant
dropdown; the listing also sells 3 V, 6 V and 24 V. If the listing offers a
tubing choice, BPT outlasts silicone under peristaltic squeeze — relevant
because throughput drops as tube flattens, and every drop means recalibrating
(`cfg.pump[i].mlPerSec`).

---

## 2. Core electronics

| Part | Qty | Spec | Firmware ref |
|---|---|---|---|
| SSD1306 OLED display | 1 | **128×64**, I²C, address **0x3C**, 3.3 V | `Adafruit_SSD1306 oled(128, 64, …)`, `ADDR_OLED` |
| ADS1115 ADC breakout | 1 | 16-bit, I²C, address **0x48** (ADDR pin → GND), run at **3.3 V** | `ADDR_ADS1115`, `GAIN_ONE`, `RATE_ADS1115_128SPS` |
| Rotary encoder w/ push switch | 1 | EC11 type, 20 detents; no external pull-ups needed | `PIN_ENC_A/B/SW`, all `INPUT_PULLUP` |
| I²C pull-up resistors | 0–2 | 4.7 kΩ to 3.3 V — **only if** your OLED/ADS breakouts lack them (most have them) | bus runs at `I2C_CLOCK_HZ` = 100 kHz |

The bus is deliberately 100 kHz, not 400 kHz: down jumper wires the rise times
at 400 kHz don't make it and the bus reports `ESP_ERR_INVALID_STATE` rather
than failing cleanly.

---

## 3. Pump drive — ×4 channels

One set per pump, on D3, D7, D8, D9.

| Part | Qty | Spec | Why |
|---|---|---|---|
| Logic-level N-channel MOSFET | 4 | **AO3400** or **IRLB8721** | A plain IRF540N will not fully turn on from 3.3 V gate drive |
| Gate pull-down resistor | 4 | **100 kΩ**, gate → GND | **Do not omit.** Holds the gate low through reset and boot, before any code runs. Without it a pump can run for a few hundred ms at power-up — a real, uncommanded dose into the reservoir |
| Gate series resistor | 4 | **100 Ω**, MCU pin → gate | Limits gate-charge current spikes |
| Flyback diode | 4 | **1N5822** (Schottky) or 1N4007, cathode to +12 V | Clamps the motor's inductive kick |

A ready-made **4-channel logic-level MOSFET module** replaces this whole
section — but verify it actually has gate pull-downs, and that its FETs are
logic-level. Many cheap modules are neither.

---

## 4. Power

| Part | Qty | Spec | Firmware ref / note |
|---|---|---|---|
| PSU, 12 V DC | 1 | **3 A** minimum, barrel or screw terminal | Firmware runs **one pump at a time** — requests queue rather than run in parallel — so 3 A covers one motor plus margin |
| Buck converter | 1 | 12 V → **5 V**, ≥1 A, into the XIAO `5V` pin | — |
| Electrolytic capacitor | 1 | **470–1000 µF**, ≥25 V, across the 12 V rail near the FETs | Absorbs motor inrush |

**Do not power the board from USB and the buck converter at the same time.**
Every boot prints its reset reason; `BROWNOUT` is the symptom of exactly that
mistake (`resetReasonText()` in `src/main.cpp`).

Star the grounds at the PSU. Pump return current sharing a ground path with the
sensor board lands directly on the pH reading as noise.

---

## 5. Sensors and analog front end

| Part | Qty | Spec | Firmware ref |
|---|---|---|---|
| pH probe + interface board | 1 | BNC probe, PH4502C-style board, **5 V supply** | item 1.2 above; defaults `phVolt7 = 2.50 V`, `phVolt4 = 3.05 V` |
| EC / TDS probe + interface board | 1 | analog output, 5 V supply | `ADS_CH_EC = 1`, `cfg.ecCalFactor` |
| Divider resistors | 4 | **10 kΩ**, two per analog channel | `ADC_DIVIDER_RATIO = 2.0f` |

Both sensor boards run at 5 V and swing 0–5 V. The ADS1115 runs at 3.3 V, so
each analog output goes through a 2:1 divider into A0 / A1:

```
sensor out ──[10k]──┬──[10k]── GND
                    └── ADS1115 A0 / A1
```

That puts at most 2.5 V on the ADS input, comfortably inside its 3.3 V supply
rail. `ADC_DIVIDER_RATIO` multiplies readings back so every calibration figure
stays in the sensor's own 0–5 V domain. If you ever power the ADS1115 from 5 V
and drive it directly, set that constant to `1.0`.

**No level shifter is needed** — both I²C devices are 3.3 V parts sitting on a
3.3 V bus. (Earlier revisions of this design used one; the code and this BOM
do not.)

---

## 6. Optional but recommended

| Part | Qty | Spec | Firmware ref |
|---|---|---|---|
| Reservoir float switch | 1 | normally-closed, on D10 | `PIN_FLOAT`, `cfg.floatEnabled`, `cfg.floatActiveLow` (default true) |
| USB-TTL serial adapter | 1 | 3.3 V logic, to **D6 (TX) only** — leave its VCC disconnected | ROM/bootloader log always exits on UART0 TX |

D7 is Pump 2's gate, so there is **no UART0 RX** available — the debug port is
listen-only. Arduino `Serial` goes over USB-C by default
(`ARDUINO_USB_CDC_ON_BOOT=1`).

Set `ENCODER_ENABLED 0` in `include/config.h` while the encoder is unwired —
floating encoder pins generate an interrupt storm. The firmware detects it and
detaches the ISRs after one second (`ENC_ISR_STORM_PER_SEC`), but not wiring it
and not disabling it wastes a second of every boot.

---

## 7. Plumbing and consumables

| Part | Qty | Spec |
|---|---|---|
| Silicone tubing | ~2 m | to match pump head bore |
| Dosing bottles / reservoirs | 4 | one per nutrient |
| pH 7.00 buffer solution | 1 | two-point calibration, first point |
| pH 4.00 buffer solution | 1 | two-point calibration, second point |
| EC standard solution | 1 | **1413 µS/cm** — the firmware's default `ecCalStandard` |
| Measuring cylinder | 1 | 25–50 ml, for pump calibration (*Run 20s* → measure → enter) |

---

## 8. Enclosure / 3D printed parts

> **Not yet available.** No STL, STEP, 3MF or other CAD files exist in this
> repository — not on `main`, not on the feature branch, and not anywhere in
> git history. This section is a placeholder; see the note in the task
> discussion.

| Part | Qty | Material | File |
|---|---|---|---|
| _TBD_ | | | |

Once the models are added, `docs/models/` alongside this file is the natural
home, with print settings (layer height, infill, supports, material) recorded
per part.

---

## 9. Fasteners and wiring

Not specified by the firmware — driven entirely by the enclosure design, so
this fills in once section 8 lands. Expect to need:

- M3 screws and heat-set inserts for the enclosure
- JST-XH or screw terminals for pump and sensor connections
- Silicone hook-up wire — 20 AWG for the 12 V pump runs, 26 AWG for signals
- Panel-mount barrel jack for the 12 V input
- Knob for the rotary encoder
