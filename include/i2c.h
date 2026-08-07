#pragma once
#include <Arduino.h>

// Defined in main.cpp. Both the display and the ADC probe their address with
// this before handing over to their Adafruit driver: neither library checks
// whether its init writes were acknowledged, so a device that is not on the
// bus otherwise floods the console with i2cWrite() errors and, in the
// display's case, still reports success.
bool i2cProbe(uint8_t addr);
