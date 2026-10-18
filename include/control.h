#pragma once
#include <Arduino.h>
#include "settings.h"

class Control {
public:
  void begin();
  void tick();

  void setPaused(bool p);
  bool paused() const { return _paused; }

  // Human-readable one-liner for the display status row and the web UI.
  const char* statusText() const { return _status; }

  uint32_t nextScheduledIn(uint8_t idx) const;  // seconds, 0 if not scheduled

private:
  bool     _paused = false;
  char     _status[40] = "Idle";
  uint32_t _nextEval = 0;
  uint32_t _mixUntil = 0;
  uint32_t _lastSched[NUM_PUMPS] = {0};

  void evalScheduled(uint8_t i);
  void evalAuto(uint8_t i);

  bool autoWants(uint8_t i) const;                   // out of band, wants a dose
  bool nextWait(uint8_t& idx, uint32_t& sec) const;  // soonest pump we wait on
  void setWait(const char* what, uint32_t sec);
};

extern Control control;
