# Carrier PCB — schematic reference for EasyEDA

One board that everything plugs into: the XIAO ESP32-C5 in a socket, the
2.0" TFT + encoder combo module on a 12-pin header, and screw/JST
connectors for pumps, sensors and power. Everything that used to be a
breakout board or a flying part — MCP23017, ADS1115, MOSFET array,
dividers, pull-ups, 5 V regulation — lives on the PCB.

Every lesson from the first bring-up is designed in: encoder RC filters
(the interrupt-storm fix), bare-chip pull-ups the breakouts used to
provide, a USB-vs-PSU OR-ing diode that retires the BROWNOUT footgun,
and the MCP23017 gate pull-downs that keep pumps off during boot.

```
12V DC ──[rev.pol]──┬────────────────────────────► 4x pump + (12V)
                    │
                    ├──[LM2596S-5.0 buck]── +5V ──┬─[SS34]─► XIAO 5V pin
                    │                             ├─► pH module, EC module
                    └── 470–1000 µF bulk          └─[AMS1117-3.3]── +3V3P
                                                       │
                        display, encoder pull-ups, MCP23017,
                        ADS1115, DS18B20, I²C pull-ups ◄┘
```

`+3V3P` is the peripheral rail. The XIAO's own regulator powers only the
MCU; the display backlight and every peripheral chip hang on the
AMS1117 instead, so WiFi current spikes and backlight load never share
a regulator. Do **not** join the XIAO's 3V3 pin to +3V3P — two regulator
outputs must never be tied together. Leave the XIAO 3V3 pin as a test
point.

---

## 1. Power input and rails

| Ref | Part | Value / package | Notes |
|---|---|---|---|
| J1 | Barrel jack DC-005 | 12 V in | plus optional 2-pin screw terminal in parallel |
| D1 | Schottky | SS54 (SMA) | series reverse-polarity protection; ~0.4 V drop at 1 A is the price of idiot-proofing |
| C1 | Electrolytic | 470–1000 µF / 25 V | bulk, physically next to the MOSFET bank |
| C2 | Ceramic | 100 nF / 0805 | across C1 |
| U1 | Buck regulator | **LM2596S-5.0**, TO-263-5 | fixed 5 V, 3 A; pin 1 VIN, 2 SW, 3 GND, 4 FB, 5 ON̅ |
| C3 | Electrolytic | 100 µF / 25 V | at U1 VIN |
| L1 | Power inductor | 33 µH, ≥3 A sat, shielded | SW → +5V |
| D2 | Schottky | SS54 | catch diode: cathode to SW node, anode to GND |
| C4 | Electrolytic | 330–470 µF / 16 V | at +5V output; FB (pin 4) connects to +5V here |
| — | U1 pin 5 (ON̅) | → GND | always on |
| D3 | Schottky | SS34 (SMA) | **OR-ing diode**: +5V → D3 → XIAO 5V pin. USB and the 12 V supply can now be connected at the same time — whichever is higher wins, neither backfeeds. This retires the "never both" rule; the XIAO sees ~4.6 V, fine for its LDO |
| U2 | LDO | AMS1117-3.3, SOT-223 | +5V in → **+3V3P** out |
| C5, C6 | Ceramic/tantalum | 10 µF in, 22 µF out | AMS1117 stability caps |
| LED1 + R1 | any 0805 LED + 2k2 | +5V → R1 → LED1 → GND | power indicator |

Budget: one pump at a time ≤0.5 A on 12 V; +5V peaks ~0.7 A (XIAO+WiFi
through the OR diode, both 5 V sensor modules); +3V3P ~100 mA (display
with backlight, logic).

## 2. XIAO ESP32-C5 socket (U3)

Two 7-pin 2.54 mm female headers. Orient so the USB-C stays reachable —
it is still the flashing and debug port.

| Pad | GPIO | Net |
|---|---|---|
| D0 | 1 | ENC_SW |
| D1 | 0 | ENC_A |
| D2 | 25 | ENC_B |
| D3 | 7 | TFT_CS |
| D4 | 23 | I2C_SDA |
| D5 | 24 | I2C_SCL |
| D6 | 11 | DBG_TX (header J8) |
| D7 | 12 | TFT_DC |
| D8 | 8 | TFT_SCK |
| D9 | 9 | ONEWIRE |
| D10 | 10 | TFT_MOSI |
| 5V | — | from D3 (OR diode) |
| GND | — | GND |
| 3V3 | — | **not connected** (test point only) |

## 3. Display + encoder header (J2, 12-pin female, 2.54 mm)

Matches the combo module edge: GND VDD SCL SDA RES DC CS BLK TRA TRB
PSH KO.

| Module pin | Net | On-board support parts |
|---|---|---|
| GND | GND | |
| VDD | +3V3P | solder jumper **SJ1** selects +3V3P (default) or +5V, for module revisions with an onboard regulator |
| SCL | TFT_SCK (D8) | this is SPI clock despite the name |
| SDA | TFT_MOSI (D10) | SPI data despite the name |
| RES | +3V3P | via R2 10 k (firmware never drives reset) |
| DC | TFT_DC (D7) | |
| CS | TFT_CS (D3) | |
| BLK | +3V3P | backlight always on |
| TRA | ENC_A (D1) | R3 10 k → +3V3P, C7 10 nF → GND |
| TRB | ENC_B (D2) | R4 10 k → +3V3P, C8 10 nF → GND |
| PSH | ENC_SW (D0) | R5 10 k → +3V3P, C9 10 nF → GND |
| KO | solder jumper **SJ2** → GND | open by default (KO is the module's spare button); close it if a module revision uses KO as encoder common |

The three RC networks are the permanent fix for the encoder interrupt
storms: stiff 10 k pull-ups instead of the MCU's weak internal ones,
and 10 nF swallowing both contact bounce and coupled SPI noise. Place
them at the MCU end of the traces.

## 4. I²C bus and expander chips

Bare chips now, so the pull-ups the breakout boards used to carry live
here: **R6, R7 = 4.7 k from I2C_SDA and I2C_SCL to +3V3P**. One pair
only, for the whole bus. 100 kHz stays the firmware's bus speed, and on
a proper PCB it will be clean at that.

### U4 — MCP23017-E/SO (SOIC-28W), address 0x20

| Pin | Name | Net |
|---|---|---|
| 1 | GPB0 | FLOAT_SW → J7 pin 1 |
| 2–8 | GPB1–7 | NC (optionally break GPB1–3 out to spare pads) |
| 9 | VDD | +3V3P (+ C10 100 nF to GND, at the pin) |
| 10 | VSS | GND |
| 11 | NC | — |
| 12 | SCL | I2C_SCL |
| 13 | SDA | I2C_SDA |
| 15,16,17 | A0,A1,A2 | GND (→ address 0x20) |
| 18 | RESET | **+3V3P via R8 10 k — do not leave floating**; breakout boards did this for you, a bare chip will not run without it |
| 19,20 | INTB, INTA | NC |
| 21–24 | GPA0–GPA3 | GATE1–GATE4 |
| 25–28 | GPA4–7 | NC |

J7 (float switch): 2-pin JST-XH — GPB0 and GND. The firmware uses the
MCP's internal pull-up; polarity is a settings toggle.

### U5 — ADS1115IDGS (MSOP-10), address 0x48

| Pin | Name | Net |
|---|---|---|
| 1 | ADDR | GND (→ 0x48) |
| 2 | ALERT | NC |
| 3 | GND | GND |
| 4 | AIN0 | PH_DIV |
| 5 | AIN1 | EC_DIV |
| 6,7 | AIN2, AIN3 | GND |
| 8 | VDD | +3V3P (+ C11 100 nF at the pin) |
| 9 | SDA | I2C_SDA |
| 10 | SCL | I2C_SCL |

## 5. Analog front end

Sensor modules stay external boards (they carry the BNC and trimmers);
they plug in with 3-pin JST-XH and are powered from **+5V**.

```
J3 pH module:  1 = +5V   2 = GND   3 = PH_RAW
J4 EC module:  1 = +5V   2 = GND   3 = EC_RAW

PH_RAW ──[R9 10k]──┬──[R10 10k]── GND        (identical for EC:
                   ├── C12 100 nF ── GND      R11/R12/C13 → EC_DIV)
                   └── PH_DIV → ADS AIN0
```

The 2:1 divider matches `ADC_DIVIDER_RATIO = 2.0` in the firmware; the
100 nF makes each tap a ~300 Hz RC filter. Use 1% resistors so the two
channels match.

J5 (DS18B20 probe): 3-pin JST-XH — 1 = +3V3P, 2 = ONEWIRE (D9),
3 = GND, with **R13 4.7 k from ONEWIRE to +3V3P** on the board.

## 6. Pump drivers ×4 (Q1–Q4)

Per channel n (1…4):

```
GATEn (MCP GPAn) ──[100R]──┬── gate  Q_n  AO3400A (SOT-23)
                           └──[100k]── GND      source ── GND
J6n pump connector: 1 = +12V   2 = drain
Flyback D4n (SS34/SS54) across 1–2, cathode to +12V
Optional: LED + 2k2 in series from +12V to drain — lights when running
```

The 100 k pull-downs are load-bearing safety: from power-on until
`pumps.begin()` brings I²C up, they are the only thing holding the
pumps off, and the MCP23017 wakes high-impedance so they win. Never
substitute a PCF8574 footprint here — its outputs wake high.

Pump connectors: JST-XH-2 or 5.08 mm screw terminals.

## 7. Debug header (J8)

3 pins: DBG_TX (D6) · GND · key/NC. UART0 TX only — D7 is the TFT's DC
line, so there is no RX; the ROM bootloader always logs here even when
USB is dead.

## 8. Layout rules — the ones that bite

1. **Star the grounds.** One ground plane is fine, but pump return
   current must flow connector → MOSFET source → C1 → jack without
   crossing under the ADS1115, the dividers, or the I²C bus. Put power
   (jack, buck, FET bank, pump connectors) on one edge, analog (ADS +
   dividers + sensor connectors) on the opposite edge, the XIAO and
   display header between.
2. **Buck loop tight.** C3 → U1 → L1 → C4 → D2 in millimetres, plane
   under it unbroken. 150 kHz is forgiving, but not of a 5 cm loop.
3. **12 V and pump traces ≥1.5 mm**; 5 V rail ≥1 mm.
4. Encoder RC parts near the XIAO, not the header; TFT SPI traces short
   and away from TRA/TRB.
5. 100 nF at every IC VDD pin, at the pin.
6. M3 mounting holes ×4; USB-C aperture clear; display header
   positioned so the module overhangs the board edge if the enclosure
   wants the glass flush.

## 9. Consolidated parts list

| Qty | Part | Package | Ref |
|---|---|---|---|
| 1 | LM2596S-5.0 | TO-263-5 | U1 |
| 1 | AMS1117-3.3 | SOT-223 | U2 |
| 1 | MCP23017-E/SO | SOIC-28W | U4 |
| 1 | ADS1115IDGS | MSOP-10 | U5 |
| 4 | AO3400A | SOT-23 | Q1–Q4 |
| 2 | SS54 | SMA | D1, D2 |
| 5 | SS34 | SMA | D3, D41–D44 |
| 1 | 33 µH ≥3 A shielded inductor | 12×12 SMD | L1 |
| 1 | 470–1000 µF 25 V | radial | C1 |
| 1 | 100 µF 25 V | radial | C3 |
| 1 | 330–470 µF 16 V | radial | C4 |
| 2 | 10 µF + 22 µF | 0805/tant | C5, C6 |
| 5+ | 100 nF | 0805 | C2, C10–C13, per-IC |
| 3 | 10 nF | 0805 | C7–C9 |
| 4 | 10 k 1% | 0805 | R9–R12 (dividers) |
| 6 | 10 k | 0805 | R2–R5, R8, + spare |
| 2 | 4.7 k | 0805 | R6, R7 (I²C) — plus R13 for 1-Wire |
| 4 | 100 R | 0805 | gate series |
| 4 | 100 k | 0805 | gate pull-downs |
| 1 | DC-005 barrel jack | THT | J1 |
| 2 | 7-pin 2.54 female header | THT | U3 socket |
| 1 | 12-pin 2.54 female header | THT | J2 |
| 4 | JST-XH-2 / screw 5.08 | THT | J61–J64 |
| 3 | JST-XH-3 | THT | J3, J4, J5 |
| 1 | JST-XH-2 | THT | J7 |
| 1 | 3-pin 2.54 header | THT | J8 |

All passives 0805 for hand-soldering. In EasyEDA, search LCSC by the
part names above and prefer **Basic** parts where offered; the XIAO
footprint is in the user-contributed libraries (any "Seeed XIAO"
footprint fits — the C5 uses the same 14-pad edge layout).

## 10. First power-up of the finished board

1. Nothing socketed. 12 V in → measure +5V (4.9–5.1) and +3V3P
   (3.25–3.35). Check U1 and U2 stay cool for a minute.
2. Power off, socket the XIAO alone, USB unplugged → 12 V in → boot log
   on J8; confirm `[i2c]` finds 0x20 and 0x48 — that is the bare chips
   and their pull-ups proving themselves.
3. Plug the display module → full UI.
4. One pump channel at a time: `Prime 10s` with a meter on the
   connector.
5. Sensors last, calibrate per README.

The firmware needs **zero changes** for this board — every net matches
`include/config.h` as it stands.
