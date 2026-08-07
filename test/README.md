# test/

PlatformIO unit tests. Empty today.

Each test suite is its own subdirectory named `test_*`, built and run
independently:

```
test/
└── test_dosing/
    └── test_dosing.cpp
```

```bash
pio test                              # on the connected board
pio test -e seeed_xiao_esp32c5 -f test_dosing
```

Worth knowing before adding tests here: the firmware's logic is reached through
global singletons that touch hardware in their constructors' wake
(`Wire`, the ADS1115, GPIO), so most of it can only be tested on-target. The
parts that are pure arithmetic — `Sensors::phFromVolts()`,
`Sensors::ecFromVolts()`, the ml → milliseconds conversion in
`Pumps::request()`, and the rolling hourly window in `Pumps::dosesLastHour()` —
are the ones worth extracting into `lib/` first so they can also run under a
`native` environment with no board attached.

More: https://docs.platformio.org/page/advanced/unit-testing/index.html
