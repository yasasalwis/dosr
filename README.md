# dosr — Mini Feeder

Four-channel 12 V dosing controller on a Seeed XIAO ESP32-C5, with a 2.0"
colour TFT + rotary encoder menu, a phone-friendly web panel, and Home
Assistant over MQTT.

Each pump runs in one of four modes, switchable per pump at runtime:

| Mode | Behaviour |
|---|---|
| Off | Pump ignored; dose requests are refused |
| Manual | Only doses when you ask, from the menu, web page, or MQTT |
| Scheduled | Doses `doseMl` every `intervalMin` minutes |
| Auto | Doses when pH or EC drifts outside its deadband |

Pump names are set from the web page. Naming things with a rotary encoder is
miserable, so the display only shows them.

## Build

```bash
pip install -U platformio        # or use the PlatformIO IDE extension
git clone <this repo> && cd dosr

pio run                          # compile
pio run -t upload                # compile and flash
pio device monitor               # 115200 baud
pio run -t clean
```

The first build pulls the RISC-V toolchain and the Arduino-ESP32 framework —
a few hundred MB of downloads, around 2.5 GB unpacked, and several minutes.
Everything after that is incremental. It needs to reach both `github.com` and
`*.platformio.org`; behind a restrictive egress proxy the give-away is the
build dying on `tool-scons`, which is served only from the PlatformIO
registry.

`platformio.ini` uses the [pioarduino] fork of the ESP32 platform, not the
official `espressif32` one and not `platform-seeedboards`. That is deliberate:
the Seeed platform v1.0.0 pins a GCC 8.4 toolchain against an IDF 5.5
framework and dies with `unrecognized command line option -std=gnu++2b`. The
pioarduino `stable` release carries a working `seeed_xiao_esp32c5` board
definition. Pin a specific release tag in place of `stable` if you need
reproducible builds.

[pioarduino]: https://github.com/pioarduino/platform-espressif32

### Machine-local settings

Do not edit `platformio.ini` for anything specific to your machine. Copy the
example instead:

```bash
cp platformio_override.ini.example platformio_override.ini
```

`platformio_override.ini` is gitignored and merged on top of `platformio.ini`
automatically. Use it to pin `upload_port`, raise the debug level, or — on
**Windows** — set a short `core_dir`:

```ini
[platformio]
core_dir = C:/pio
```

Windows needs that because the `esp32-arduino-libs` package ships Matter
headers whose paths exceed the 260-character limit under
`C:\Users\<name>\.platformio`; the build then fails with file-not-found on
headers that are plainly sitting there. Linux and macOS need nothing.

### Project layout

```
platformio.ini          build config: platform, board, flags, lib_deps
platformio_override.ini.example
include/                shared headers (see include/README.md)
src/                    main.cpp + one .cpp per module
lib/                    private libraries — empty for now
test/                   unit tests — empty for now
```

| Module | Responsibility |
|---|---|
| `main` | Boot, I²C scan, watchdog, `loop()` ordering |
| `settings` | `Settings` struct, NVS load/save, debounced writes |
| `pumps` | Dose queue, timing, rate limits, float-switch abort |
| `sensors` | ADS1115 sampling, temperature, pH/EC conversion, calibration |
| `ds18b20` | Bit-banged 1-Wire master for the temperature probe |
| `control` | Scheduled and closed-loop dosing decisions |
| `ui` | ST7789 menu, rotary encoder, toasts |
| `net` | WiFi/AP, HTTP API, MQTT, Home Assistant discovery |
| `webpage` | The web panel, a `PROGMEM` string in `include/webpage.h` |

`loop()` order matters: `pumps.tick()` runs first, because stopping a running
pump on time is the one deadline in the system that has a physical consequence.

There is no filesystem image — the web page is compiled into flash, so
`pio run -t upload` is the whole deployment.

## Hardware

```
12 V 3 A PSU ─┬─ buck converter ─ 5 V rail ─ XIAO 5V pin ─ 3V3 logic
              └─ MOSFET drains ─ 4 pumps
```

All eleven exposed pads are spoken for, which is why the pump gates and the
float switch sit on an I²C expander rather than on the MCU — the SPI display
needs four pins and there is nowhere else to find them. `docs/BOM.md` §2 shows
the arithmetic.

| Function | Pad | GPIO |
|---|---|---|
| Encoder switch | D0 | 1 |
| Encoder A | D1 | 0 |
| Encoder B | D2 | 25 |
| TFT CS | D3 | 7 |
| I²C SDA | D4 | 23 |
| I²C SCL | D5 | 24 |
| *(free — UART0 TX)* | D6 | 11 |
| TFT DC | D7 | 12 |
| TFT SCK | D8 | 8 |
| DS18B20 temperature | D9 | 9 |
| TFT MOSI | D10 | 10 |

On the MCP23017 at `0x20`:

| Function | Pin |
|---|---|
| Pump 1–4 gates | GPA0–GPA3 |
| Float switch | GPB0 |

D8 and D10 are the variant's native SPI SCK and MOSI, so the peripheral needs
no remapping. `SPI.begin()` is called with `miso = -1`; the core skips
`spiAttachMISO()` for a negative pin, which is what keeps GPIO9 free for the
temperature probe. TFT `RST` ties to 3V3 and `BLK` to 3V3.

I²C devices: MCP23017 at `0x20`, ADS1115 at `0x48`. The bus runs at 100 kHz —
400 kHz does not survive jumper wires, and the symptom is
`ESP_ERR_INVALID_STATE` rather than a clean timeout.

The display is SPI, not I²C. SPI has no acknowledge, so unlike the I²C parts
there is no way to tell a wired display from an absent one — `ui.begin()`
reports what it configured, not what answered.

### If the display stays black

Every entry here has produced "backlight on, screen black" on this exact
build, roughly in order of how long each one took to find:

- **A solder bridge on the module's pin header.** One blob across DC and a
  neighbour defeats everything downstream — and continuity-testing the
  jumper wires passes, because the short is on the PCB, not in the wires.
  Inspect the header under a bright light before suspecting anything else.
- **SCL/SDA on the wrong pads.** The module's SCL/SDA silkscreen means SPI
  SCK/MOSI. They go to **D8 and D10** — not to D4/D5, whose XIAO pinout
  labels (SDA/SCL) invite exactly that mistake.
- **RES left floating.** The firmware never drives reset (the constructor
  passes `-1`), so RES must be tied to 3V3 or the panel can sit in reset
  forever, backlit and blank.
- **Power that only feeds the backlight.** No glow at all means VDD/BLK/GND;
  a glow with dead logic can mean a module whose onboard regulator wants
  **5 V** on VDD — the backlight runs happily off 3.3 V while the panel
  logic browns out. Look for a 3-pin regulator on the module.
- **Wrong orientation, a corner-sized image, or negative colours.** The
  fitted panel is the 2.0" ST7789 (320×240). An image squeezed into one
  corner over noise means ST7735 init on ST7789 glass. Upside-down: swap
  `setRotation(1)` for `setRotation(3)` in `src/ui.cpp`. Photo-negative
  colours: add `tft.invertDisplay(false)` after `tft.init()`. If your combo
  module is the 1.8" ST7735 variant instead, use `Adafruit_ST7735` with
  `initR(INITR_BLACKTAB)` and 128×160 — see the comments in `ui.cpp`.

The DS18B20 needs a **4.7 kΩ pull-up from D9 to 3V3** — it is open-drain and
nothing else holds that line high. Conversions run asynchronously at 12-bit
resolution (0.0625 °C, 750 ms): `sensors.tick()` starts one, returns, and
collects the result on a later pass, because blocking `loop()` for a
conversion would eat into pump timing. Readings of exactly 85 °C are discarded
— that is the DS18B20's power-on default, and it looks like a plausible
reading otherwise. The bus reset at the start of each cycle doubles as the
presence check, so a probe plugged in after boot is picked up on the next
five-second tick and one removed simply reverts to the manual figure.

The probe is driven by `src/ds18b20.cpp`, not by OneWire + DallasTemperature.
OneWire's ESP32 path writes the GPIO registers directly —
`GPIO.out_w1tc = (1 << pin)` — and on the ESP32-C5 under IDF 5.5 those
registers are bitfield unions rather than `uint32_t`, so the library does not
compile at all. It picks that path on `defined(ESP32)`, with no way to reach
its portable `digitalWrite()` fallback from outside. The in-tree driver goes
through the Arduino API, which the core keeps stable across SoC revisions, and
assumes a single device on the bus so every transaction is SKIP ROM.

### No level shifter

Every I²C device here runs at 3.3 V, straight onto the ESP32's bus with no
shifter. This removes the most common source of I²C trouble on this build.

The XIAO ESP32-C5 exposes only one ADC pin on the edge headers, which is why
the pH and EC boards go through an ADS1115 instead of the chip's own ADC.

Those boards still need 5 V and still swing 0–5 V, which a 3.3 V-powered
ADS1115 cannot take. Each analog output therefore goes through a **2:1 divider
— two 10 kΩ resistors**, tapped at the junction, into ADS1115 A0 and A1:

```
sensor out ──[10k]──┬──[10k]── GND
                    └── ADS1115 A0 / A1
```

The ADC runs at `GAIN_ONE` (±4.096 V full scale) and `ADC_DIVIDER_RATIO` in
`include/config.h` multiplies readings back up, so every calibration figure
stays in the sensor's own 0–5 V domain. If you later power the ADS1115 from
5 V and drive it directly, drop the dividers and set that constant to `1.0`.

**Do not skip these:**

- 100 kΩ pull-down from every MOSFET gate to GND, plus 100 Ω in series with
  the gate. These now carry more weight than they used to. With the gates on
  an expander, firmware cannot drive them low until I²C is up, so from reset
  until `pumps.begin()` the pull-downs are the *only* thing holding the pumps
  off — and they are also what makes the deliberate reset above a safe
  response to a dead bus.
- **An MCP23017, not a PCF8574.** The MCP wakes with every pin a
  high-impedance input, so the pull-downs win. A PCF8574's quasi-bidirectional
  outputs wake *high*, which would start all four pumps at power-up.
- Flyback diode across each pump, cathode to +12 V (1N5822 or 1N4007).
- Logic-level MOSFETs — AO3400, IRLB8721, or a ready-made 4-channel module.
  A plain IRF540N will not turn on properly from 3.3 V.
- Star the grounds at the PSU. Pump current sharing a ground return with the
  sensor board shows up directly as noise on your pH reading.
- 470–1000 µF across the 12 V rail near the FETs.

## First run

1. Power up. With no WiFi credentials stored, the board starts an access
   point: **MiniFeeder-Setup / feedme1234**. Join it and open
   `http://192.168.4.1`.
2. Enter your WiFi and MQTT details at the bottom of the page, save, reboot.
3. Prime each pump (pump menu → *Prime 10s*) until the line is full of liquid.
4. Calibrate each pump before you trust any dose figure.
5. Calibrate the probes.
6. Set modes, setpoints, and rate limits.

Set `ENCODER_ENABLED 0` in `include/config.h` while the encoder is unwired.

Menu controls: rotate to move or adjust, short-press the knob to select, and
**hold the knob for half a second to go back**. While editing a value, the
first long press exits the edit; the next one leaves the screen. Every
submenu also ends with a `< Back` row, so the long press is the shortcut,
not the only way out.

If rotation stops working while the click still does, the firmware detached
a storming encoder — a toast says so, serial prints the interrupt count, and
it retries on its own every 5 s (five strikes and it stays off until
reboot). A count in the tens of thousands means TRA/TRB is floating — a
loose wire; a count near the threshold means induced noise — run the
encoder jumpers away from the display's SPI wires, and if it persists add
10 kΩ pull-ups to 3V3 and 10 nF to GND on both channels.

## Calibrating a pump

Menu → Pumps → *(pump)* → Calibrate.

1. Put the outlet in a measuring cylinder.
2. *Run 20s*.
3. Enter what you actually collected into *Measured ml* and press the encoder.

The firmware divides by 20 and stores ml/s. Every dose from then on is timed
from that number. Recalibrate when you change tubing — peristaltic tube
flattens with age and throughput drops noticeably.

## Calibrating the probes

Two-point for pH: rinse, sit the probe in pH 7.00 buffer, wait for the reading
to settle, then Menu → Sensors → *Cal in pH 7.00*. Rinse, repeat in pH 4.00.
Calibration is refused while a pump is running or settling.

EC is single-point: put the probe in a known standard, set *EC standard* to
that value (1413 µS/cm is the usual one), then *Cal EC now*.

Temperature comes from the DS18B20 on D9 when one is fitted — Menu → Sensors
shows it as *Probe temp*, and calibration records the temperature it was done
at. With no probe the compensation falls back to *Manual C*, which you then
have to set to your reservoir temperature by hand. Either way the compensation
is the standard 2 %/°C approximation, so a wrong temperature skews every
reading.

## Safety behaviour

- One pump at a time. Requests queue rather than run in parallel, so four
  motors can never hit the 12 V rail together.
- A single dose is hard-capped at 90 s regardless of settings.
- `maxDosesPerHour` per pump, enforced on a rolling one-hour window.
- Auto mode doses once, then waits `mixWaitSec` before re-evaluating.
  Without that wait the loop badly overshoots.
- Sensor readings are marked invalid while pumps run and for `settleSec`
  after. Auto mode ignores invalid and stale (>30 s) readings.
- Optional float switch on MCP23017 GPB0 aborts a dose in progress and blocks
  new ones. It is polled at 20 Hz rather than read on demand, since it now sits
  behind the I²C bus — 50 ms of latency against a dose measured in seconds.
- Every gate write is read back. If one does not take, the write is retried;
  if a *stop* keeps failing for 2 s the firmware resets the board deliberately,
  because the MCP23017 wakes with all pins high-impedance and the 100 kΩ gate
  pull-downs then cut the pumps. This is the one failure the firmware cannot
  otherwise escape: the motor is running and the only path to its gate is a bus
  that has stopped answering.
- If the MCP23017 does not answer at boot, `driverOk()` stays false and every
  dose request is refused with `pump driver offline`. Nothing can run.
- Task watchdog resets the board if `loop()` stalls for 15 s.
- MQTT socket timeout is 4 s, deliberately under the watchdog. PubSubClient
  defaults to 15 s, which turns an unreachable broker into a reboot loop.
- If the encoder pins float, the ISR storm detector disables the interrupts
  after one second rather than letting the board lock up.
- Rejected doses always return a reason — nothing fails silently.

## HTTP API

| Method | Path | Notes |
|---|---|---|
| GET | `/api/state` | live readings, temperature, status, per-pump totals |
| GET | `/api/settings` | full config; passwords are never returned |
| POST | `/api/settings` | JSON body, values clamped server-side |
| POST | `/api/dose?pump=0&ml=5` | `ml` optional, defaults to configured dose |
| POST | `/api/prime?pump=0&ms=10000` | |
| POST | `/api/stop` | stops everything, clears the queue |
| POST | `/api/pause?on=1` | |
| POST | `/api/cal/ph?value=7` | |
| POST | `/api/cal/ec?value=1413` | |
| POST | `/api/reboot` | |

There is no authentication. Keep this on a trusted VLAN.

## MQTT

Base topic is `deviceName` (default `minifeeder`).

Published: `minifeeder/state` (JSON, every 5 s), `minifeeder/status`
(`online`/`offline`, retained, also the LWT).

Subscribed: `minifeeder/pump/<0-3>/dose` (payload = ml, empty for the
configured dose), `minifeeder/pause` (`1`/`0`), `minifeeder/stop`.

Home Assistant discovery is published automatically on connect: pH, EC and
temperature sensors, a status sensor, a dose button and a total-dosed sensor per pump,
and a "Dosing paused" switch, all under one device.

## If it will not boot

`ARDUINO_USB_CDC_ON_BOOT=1`, so Arduino `Serial` — everything the firmware
prints — comes out of the USB-C port. `pio device monitor` at 115200 is all
you need in the normal case.

When USB will not enumerate at all, which is exactly when you want the log,
fall back to UART0: **TX on D6**. Wire a USB-TTL adapter's RX to D6 and its GND
to the board's GND, leave its VCC disconnected, and open the port at 115200.
There is no RX — D7 carries the TFT's DC line — so this is a listen-only
channel, which is enough. The ROM and second-stage bootloader always log here regardless of
the CDC setting.

To move Arduino `Serial` itself to UART0 as well, set
`-DARDUINO_USB_CDC_ON_BOOT=0` in `platformio_override.ini`.

Every boot prints the reset reason. `BROWNOUT` means the supply sagged — do
not run USB and the buck converter into the 5V pin at the same time. Every
boot also scans the I²C bus and lists what it found; an empty scan is a wiring
problem, not a firmware one.

## Things worth changing

- Scheduling is interval-based from boot, not wall-clock. That survives a WiFi
  outage but means "dose at 06:00" needs NTP and an RTC check in `control.cpp`.
- pH and EC probes in the same solution cross-talk through the liquid. If
  readings jump when both are submerged, power the EC board from a spare
  MOSFET channel and switch it on only for its own read window.
- Nothing is unit-tested. The pure arithmetic — `phFromVolts`, `ecFromVolts`,
  the ml → ms conversion, the rolling hourly window — would move into `lib/`
  and run under a `native` test environment without a board attached. See
  `test/README.md`.

## Licence

GPLv3 — see [LICENSE](LICENSE).
