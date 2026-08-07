#pragma once
#include <Arduino.h>
#include "config.h"
#include "settings.h"

// Reasons a dose request can be turned down. Surfaced in the UI and over MQTT
// so a rejected dose is never silent.
enum DoseResult : uint8_t {
  DOSE_QUEUED = 0,
  DOSE_REJECT_MODE,        // pump is set to Off
  DOSE_REJECT_RATE,        // maxDosesPerHour reached
  DOSE_REJECT_QUEUE_FULL,
  DOSE_REJECT_CALIBRATION, // mlPerSec is not usable
  DOSE_REJECT_FLOAT,       // reservoir float switch says empty
  DOSE_REJECT_LOCKOUT,
  DOSE_REJECT_BADARG,
  DOSE_REJECT_NO_DRIVER    // MCP23017 absent or the bus is not answering
};

const char* doseResultText(DoseResult r);

class Pumps {
public:
  void begin();
  void tick();

  // Queue a dose. Returns immediately; the pump runs from tick().
  DoseResult request(uint8_t idx, float ml, const char* source);

  // Run a pump for a fixed time, ignoring calibration. Used by the
  // "calibrate" flow where you measure what actually comes out.
  DoseResult requestTimed(uint8_t idx, uint32_t ms, const char* source);

  void stopAll();                       // immediate, also clears the queue

  bool     busy() const     { return _running >= 0; }
  int8_t   running() const  { return _running; }
  uint32_t runRemainingMs() const;
  uint8_t  queued() const   { return _qCount; }
  uint32_t lastStopMs() const { return _lastStopMs; }
  uint32_t lastDoseMs(uint8_t i) const { return _lastDoseMs[i]; }
  float    totalMl(uint8_t i) const    { return _totalMl[i]; }
  uint8_t  dosesLastHour(uint8_t i);
  bool     floatOk() const;

  // False when the MCP23017 did not answer at boot. Every dose is refused
  // while this is false - there is no way to drive a gate without it.
  bool     driverOk() const { return _driverOk; }

private:
  struct Job { uint8_t idx; uint32_t ms; float ml; };

  Job      _queue[DOSE_QUEUE_LEN];
  uint8_t  _qHead = 0, _qTail = 0, _qCount = 0;

  int8_t   _running   = -1;
  uint32_t _runUntil  = 0;
  float    _runMl     = 0;
  uint32_t _lastStopMs = 0;

  uint32_t _lastDoseMs[NUM_PUMPS]  = {0};
  float    _totalMl[NUM_PUMPS]     = {0};
  uint32_t _history[NUM_PUMPS][DOSE_HISTORY_LEN] = {{0}};
  uint8_t  _histIdx[NUM_PUMPS]     = {0};

  bool     _driverOk   = false;
  uint8_t  _gates      = 0;      // shadow of MCP port A, one bit per pump
  bool     _floatOk    = true;   // cached; the switch is behind the expander
  uint32_t _nextFloatRead = 0;
  uint32_t _stopFailSince = 0;   // 0 = the last gate write succeeded

  bool push(const Job& j);
  void startNext();
  void stopCurrent();
  void recordDose(uint8_t idx);

  bool writeGates(uint8_t mask);   // write port A and verify by read-back
  void readFloat();
};

extern Pumps pumps;
