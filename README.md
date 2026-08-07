# dosr — Mini Feeder

Four-channel 12 V dosing controller on a Seeed XIAO ESP32-C5, with an OLED +
rotary encoder menu, a phone-friendly web panel, and Home Assistant over MQTT.

Each pump runs in one of four modes, switchable per pump at runtime:

| Mode | Behaviour |
|---|---|
| Off | Pump ignored; dose requests are refused |
| Manual | Only doses when you ask, from the menu, web page, or MQTT |
| Scheduled | Doses `doseMl` every `intervalMin` minutes |
| Auto | Doses when pH or EC drifts outside its deadband |

Pump names are set from the web page. Naming things with a rotary encoder is
miserable, so the OLED only displays them.

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
| `sensors` | ADS1115 sampling, pH/EC conversion, calibration |
| `control` | Scheduled and closed-loop dosing decisions |
| `ui` | SSD1306 menu, rotary encoder, toasts |
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

| Function | Pad | GPIO |
|---|---|---|
| Encoder switch | D0 | 1 |
| Encoder A | D1 | 0 |
| Encoder B | D2 | 25 |
| Pump 1 gate | D3 | 7 |
| I²C SDA | D4 | 23 |
| I²C SCL | D5 | 24 |
| *(free — UART0 TX)* | D6 | 11 |
| Pump 2 gate | D7 | 12 |
| Pump 3 gate | D8 | 8 |
| Pump 4 gate | D9 | 9 |
| Float switch | D10 | 10 |

I²C devices: SSD1306 OLED at `0x3C`, ADS1115 at `0x48`. The bus runs at
100 kHz — 400 kHz does not survive jumper wires, and the symptom is
`ESP_ERR_INVALID_STATE` rather than a clean timeout.

### No level shifter

The ADS1115 and the OLED both run at 3.3 V, straight onto the ESP32's bus with
no shifter. This removes the most common source of I²C trouble on this build.

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
  the gate. Without the pull-down a pump can run for a few hundred
  milliseconds at power-up, which is a real dose into your reservoir.
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
that value (1413 µS/cm is the usual one), then *Cal EC now*. Set *Solution C*
to your reservoir temperature — the compensation is the standard 2 %/°C
approximation, so a wrong temperature skews every reading.

## Safety behaviour

- One pump at a time. Requests queue rather than run in parallel, so four
  motors can never hit the 12 V rail together.
- A single dose is hard-capped at 90 s regardless of settings.
- `maxDosesPerHour` per pump, enforced on a rolling one-hour window.
- Auto mode doses once, then waits `mixWaitSec` before re-evaluating.
  Without that wait the loop badly overshoots.
- Sensor readings are marked invalid while pumps run and for `settleSec`
  after. Auto mode ignores invalid and stale (>30 s) readings.
- Optional float switch on D10 aborts a dose in progress and blocks new ones.
- Task watchdog resets the board if `loop()` stalls for 15 s.
- MQTT socket timeout is 4 s, deliberately under the watchdog. PubSubClient
  defaults to 15 s, which turns an unreachable broker into a reboot loop.
- If the encoder pins float, the ISR storm detector disables the interrupts
  after one second rather than letting the board lock up.
- Rejected doses always return a reason — nothing fails silently.

## HTTP API

| Method | Path | Notes |
|---|---|---|
| GET | `/api/state` | live readings, status, per-pump totals |
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

Home Assistant discovery is published automatically on connect: pH and EC
sensors, a status sensor, a dose button and a total-dosed sensor per pump,
and a "Dosing paused" switch, all under one device.

## If it will not boot

`ARDUINO_USB_CDC_ON_BOOT=1`, so Arduino `Serial` — everything the firmware
prints — comes out of the USB-C port. `pio device monitor` at 115200 is all
you need in the normal case.

When USB will not enumerate at all, which is exactly when you want the log,
fall back to UART0: **TX on D6**. Wire a USB-TTL adapter's RX to D6 and its GND
to the board's GND, leave its VCC disconnected, and open the port at 115200.
There is no RX — D7 is pump 2's gate — so this is a listen-only channel, which
is enough. The ROM and second-stage bootloader always log here regardless of
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
- EC temperature is entered by hand. A DS18B20 on a spare pin and real
  compensation would be the single biggest accuracy win.
- pH and EC probes in the same solution cross-talk through the liquid. If
  readings jump when both are submerged, power the EC board from a spare
  MOSFET channel and switch it on only for its own read window.
- Nothing is unit-tested. The pure arithmetic — `phFromVolts`, `ecFromVolts`,
  the ml → ms conversion, the rolling hourly window — would move into `lib/`
  and run under a `native` test environment without a board attached. See
  `test/README.md`.

## Licence

GPLv3 — see [LICENSE](LICENSE).
