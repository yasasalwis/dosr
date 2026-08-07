#pragma once
#include <Arduino.h>

struct Reading {
  float    ph        = NAN;
  float    ec        = NAN;   // uS/cm, temperature compensated to 25 C
  float    phVolt    = NAN;
  float    ecVolt    = NAN;
  float    tempC     = NAN;   // from the DS18B20, NAN if there isn't one
  bool     tempValid = false;
  bool     valid     = false; // false while pumps are running or settling
  uint32_t ts        = 0;
};

class Sensors {
public:
  bool begin();
  void tick();

  const Reading& last() const { return _last; }
  bool  present() const { return _present; }
  bool  settling() const;

  // Stores the current raw voltage as the calibration point for the given
  // buffer solution. Returns false if the reading is not currently trustworthy.
  bool calibratePh(float knownPh);       // pass 4.0 or 7.0
  bool calibrateEc(float knownUsPerCm);  // e.g. 1413.0

  float phFromVolts(float v) const;
  float ecFromVolts(float v) const;

  // The temperature EC compensation actually uses: the probe when it is
  // healthy, the hand-entered cfg.tempC when it is not. Unplugging the probe
  // falls back rather than failing.
  float tempC() const;
  bool  tempProbeOk() const { return _tempOk; }

private:
  Reading _last;
  bool     _present = false;
  uint32_t _nextRead = 0;

  // DS18B20. A 12-bit conversion takes 750 ms, which is far too long to block
  // loop() for, so requests and reads are split across ticks.
  bool     _tempOk      = false;
  bool     _converting  = false;
  uint32_t _tempNext    = 0;
  uint32_t _convDoneAt  = 0;
  uint8_t  _dsAddr[8]   = {0};

  float readChannelVolts(uint8_t ch, uint8_t samples = 9);
  void  tempBegin();
  void  tempTick();
};

extern Sensors sensors;
