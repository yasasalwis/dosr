#include "ds18b20.h"

namespace ds18b20 {
namespace {

uint8_t busPin = 0xFF;

const uint8_t CMD_SKIP_ROM        = 0xCC;
const uint8_t CMD_CONVERT_T       = 0x44;
const uint8_t CMD_READ_SCRATCH    = 0xBE;
const uint8_t CMD_WRITE_SCRATCH   = 0x4E;

// The pin sits in OUTPUT_OPEN_DRAIN for its whole life. Driving it low pulls
// the bus down; writing high releases it to the external pull-up. ESP32's
// OUTPUT is 0x03, which carries the INPUT bit, so the input buffer stays live
// and the line can be sampled without ever touching pinMode() again - which
// matters, because pinMode() is far too slow for a 6 us time slot.
inline void low()      { digitalWrite(busPin, LOW); }
inline void release()  { digitalWrite(busPin, HIGH); }
inline uint8_t sample() { return digitalRead(busPin); }

// Timings are the "recommended" figures from Maxim AN126. Interrupts are
// masked only across the parts that actually have a deadline: a late release
// or a late sample corrupts the bit, while a stretched low pulse does not.
bool reset() {
  low();
  delayMicroseconds(480);

  noInterrupts();
  release();
  delayMicroseconds(70);
  bool present = (sample() == LOW);   // the device answers by pulling low
  interrupts();

  delayMicroseconds(410);
  return present;
}

void writeBit(bool bit) {
  noInterrupts();
  low();
  delayMicroseconds(bit ? 6 : 60);
  release();
  interrupts();
  delayMicroseconds(bit ? 64 : 10);
}

bool readBit() {
  noInterrupts();
  low();
  delayMicroseconds(6);
  release();
  delayMicroseconds(9);
  bool b = (sample() != LOW);
  interrupts();
  delayMicroseconds(55);
  return b;
}

void writeByte(uint8_t v) {
  for (uint8_t i = 0; i < 8; i++) { writeBit(v & 0x01); v >>= 1; }
}

uint8_t readByte() {
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; i++) if (readBit()) v |= (1 << i);
  return v;
}

// Dallas/Maxim CRC-8, polynomial x^8 + x^5 + x^4 + 1 in its reflected form.
uint8_t crc8(const uint8_t* d, uint8_t len) {
  uint8_t crc = 0;
  while (len--) {
    uint8_t in = *d++;
    for (uint8_t i = 8; i; i--) {
      uint8_t mix = (crc ^ in) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      in >>= 1;
    }
  }
  return crc;
}

}  // namespace

void begin(uint8_t pin) {
  busPin = pin;
  pinMode(busPin, OUTPUT_OPEN_DRAIN);
  release();
}

bool startConversion() {
  if (busPin == 0xFF || !reset()) return false;
  writeByte(CMD_SKIP_ROM);
  writeByte(CMD_CONVERT_T);
  return true;
}

bool setResolution12() {
  if (busPin == 0xFF || !reset()) return false;
  writeByte(CMD_SKIP_ROM);
  writeByte(CMD_WRITE_SCRATCH);
  writeByte(0x4B);   // TH alarm, unused
  writeByte(0x46);   // TL alarm, unused
  writeByte(0x7F);   // config: 12-bit
  return true;
}

bool readTemperature(float& outC) {
  if (busPin == 0xFF || !reset()) return false;
  writeByte(CMD_SKIP_ROM);
  writeByte(CMD_READ_SCRATCH);

  uint8_t sp[9];
  for (uint8_t i = 0; i < 9; i++) sp[i] = readByte();

  if (crc8(sp, 8) != sp[8]) return false;

  // An idle bus reads back as all 0x00 with a CRC of 0x00, which passes the
  // check above and decodes to a perfectly plausible 0.0 C. Reject it.
  bool allZero = true;
  for (uint8_t i = 0; i < 9; i++) if (sp[i]) { allZero = false; break; }
  if (allZero) return false;

  int16_t raw = (int16_t)(((uint16_t)sp[1] << 8) | sp[0]);

  // Undefined low bits at reduced resolution, in case the probe came out of
  // someone else's project still configured for 9, 10 or 11 bits.
  switch (sp[4] & 0x60) {
    case 0x00: raw &= ~7; break;   //  9-bit
    case 0x20: raw &= ~3; break;   // 10-bit
    case 0x40: raw &= ~1; break;   // 11-bit
    default:              break;   // 12-bit
  }

  outC = raw * 0.0625f;
  return true;
}

}  // namespace ds18b20
