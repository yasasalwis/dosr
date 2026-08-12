# include/

Project header files. PlatformIO puts this directory on the compiler's include
path for every source file in `src/` and for every private library in `lib/`,
so `#include "pumps.h"` resolves from anywhere in the project.

| Header | What lives here |
|---|---|
| `config.h` | Pin map, I²C addresses, hard safety ceilings, firmware version |
| `i2c.h` | `i2cProbe()` — bus presence check used before each driver's `begin()` |
| `settings.h` | `Settings`/`PumpCfg` structs and the NVS load/save API |
| `pumps.h` | Dose queue, rate limiting, `DoseResult` reasons |
| `sensors.h` | ADS1115 sampling, pH/EC conversion and calibration |
| `control.h` | Scheduled and closed-loop dosing decisions |
| `ui.h` | TFT menu and rotary encoder |
| `net.h` | WiFi, HTTP API, MQTT |
| `webpage.h` | The single-page web panel, as a `PROGMEM` string |

Anything not shared between translation units stays `static` inside the
matching `.cpp` in `src/`.

More on the layout:
https://docs.platformio.org/page/projectconf/sections/platformio/options/directory/include_dir.html
