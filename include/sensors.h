#pragma once
#include <Arduino.h>

struct Reading {
  float    ph      = NAN;
  float    ec      = NAN;   // uS/cm, temperature compensated to 25 C
  float    phVolt  = NAN;
  float    ecVolt  = NAN;
  bool     valid   = false; // false while pumps are running or settling
  uint32_t ts      = 0;
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

private:
  Reading _last;
  bool     _present = false;
  uint32_t _nextRead = 0;

  float readChannelVolts(uint8_t ch, uint8_t samples = 9);
};

extern Sensors sensors;
