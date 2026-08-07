#pragma once
#include <Arduino.h>

// Minimal single-device DS18B20 driver.
//
// This exists instead of paulstoffregen/OneWire + DallasTemperature because
// OneWire's fast path writes the ESP32 GPIO registers directly:
//
//     GPIO.out_w1tc = ((uint32_t)1 << pin);
//
// On the ESP32-C5 under IDF 5.5 those registers are bitfield unions, not
// uint32_t, so that whole file fails to compile. The library selects its
// register path on `defined(ESP32)`, so there is no way to fall back to its
// portable digitalWrite() implementation from outside.
//
// Everything below goes through the Arduino API, which the core keeps stable
// across SoC revisions. It costs a little speed that a 1-Wire bus at 15 kbit/s
// does not care about.
//
// The bus is assumed to hold exactly one device, so every transaction uses
// SKIP ROM and there is no ROM search. Wire a 4.7 kOhm pull-up to 3V3.
namespace ds18b20 {

// Configures the pin. Does not require a probe to be present - presence is
// re-checked on every conversion, so a probe plugged in later is picked up.
void begin(uint8_t pin);

// Resets the bus and issues CONVERT T. False means nothing answered the reset.
bool startConversion();

// Reads the scratchpad and returns degrees C. False on a missing device, a bad
// CRC, or an implausible scratchpad. Call at least 750 ms after
// startConversion() - a 12-bit conversion is that slow.
bool readTemperature(float& outC);

// Normalises resolution to 12 bits. The setting lives in the probe's EEPROM,
// so a probe configured by some previous project would otherwise keep it.
bool setResolution12();

}  // namespace ds18b20
