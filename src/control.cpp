#include "control.h"
#include "pumps.h"
#include "sensors.h"

Control control;

void Control::begin() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_PUMPS; i++) _lastSched[i] = now;
  snprintf(_status, sizeof(_status), "Idle");
}

void Control::setPaused(bool p) {
  _paused = p;
  if (p) {
    pumps.stopAll();
    snprintf(_status, sizeof(_status), "Paused");
  } else {
    snprintf(_status, sizeof(_status), "Idle");
  }
}

uint32_t Control::nextScheduledIn(uint8_t idx) const {
  if (idx >= NUM_PUMPS) return 0;
  const PumpCfg& p = cfg.pump[idx];
  if (p.mode != MODE_SCHEDULED || p.intervalMin == 0) return 0;
  uint32_t period = (uint32_t)p.intervalMin * 60000UL;
  uint32_t elapsed = millis() - _lastSched[idx];
  return (elapsed >= period) ? 0 : (period - elapsed) / 1000UL;
}

void Control::evalScheduled(uint8_t i) {
  const PumpCfg& p = cfg.pump[i];
  if (p.intervalMin == 0) return;
  uint32_t period = (uint32_t)p.intervalMin * 60000UL;
  if (millis() - _lastSched[i] < period) return;

  _lastSched[i] = millis();
  DoseResult r = pumps.request(i, p.doseMl, "schedule");
  if (r == DOSE_QUEUED) {
    snprintf(_status, sizeof(_status), "%s scheduled dose", p.name);
    _mixUntil = millis() + (uint32_t)cfg.mixWaitSec * 1000UL;
  } else {
    snprintf(_status, sizeof(_status), "%s: %s", p.name, doseResultText(r));
  }
}

// Does this pump see its measurement outside the deadband right now? Split out
// of evalAuto so the wait row can tell "nothing to do" from "held off".
bool Control::autoWants(uint8_t i) const {
  const PumpCfg& p = cfg.pump[i];
  const Reading& r = sensors.last();

  if (!r.valid) return false;
  if (millis() - r.ts > 30000UL) return false;        // reading is stale

  switch (p.target) {
    case TGT_PH_DOWN: return !isnan(r.ph) && r.ph > cfg.phSetpoint + cfg.phDeadband;
    case TGT_PH_UP:   return !isnan(r.ph) && r.ph < cfg.phSetpoint - cfg.phDeadband;
    case TGT_EC_UP:   return !isnan(r.ec) && r.ec < cfg.ecSetpoint - cfg.ecDeadband;
  }
  return false;
}

void Control::evalAuto(uint8_t i) {
  const PumpCfg& p = cfg.pump[i];

  if (millis() - pumps.lastDoseMs(i) < (uint32_t)p.lockoutMin * 60000UL) return;
  if (!autoWants(i)) return;

  DoseResult res = pumps.request(i, p.doseMl, "auto");
  if (res == DOSE_QUEUED) {
    snprintf(_status, sizeof(_status), "%s correcting", p.name);
    // One correction at a time, then wait for the reservoir to mix before
    // looking again. Without this the loop will badly overshoot.
    _mixUntil = millis() + (uint32_t)cfg.mixWaitSec * 1000UL;
  } else {
    snprintf(_status, sizeof(_status), "%s: %s", p.name, doseResultText(res));
  }
}

// "Wait mix 45s", "Wait pH Down 12m" - one line, so keep the unit short.
void Control::setWait(const char* what, uint32_t sec) {
  if (sec >= 60)
    snprintf(_status, sizeof(_status), "Wait %.20s %lum", what,
             (unsigned long)((sec + 59) / 60));
  else
    snprintf(_status, sizeof(_status), "Wait %.20s %lus", what,
             (unsigned long)sec);
}

// Soonest pump the loop is holding off on: an auto pump inside its lockout
// while its reading is out of band, or the next scheduled dose.
bool Control::nextWait(uint8_t& idx, uint32_t& sec) const {
  bool found = false;
  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    uint32_t left = 0;
    if (cfg.pump[i].mode == MODE_AUTO) {
      uint32_t lock = (uint32_t)cfg.pump[i].lockoutMin * 60000UL;
      uint32_t since = millis() - pumps.lastDoseMs(i);
      if (since < lock && autoWants(i)) left = (lock - since + 999) / 1000UL;
    } else if (cfg.pump[i].mode == MODE_SCHEDULED) {
      left = nextScheduledIn(i);
    }
    if (left == 0) continue;
    if (!found || left < sec) { idx = i; sec = left; found = true; }
  }
  return found;
}

void Control::tick() {
  if (_paused) return;
  if (pumps.busy() || pumps.queued() > 0) return;
  if ((int32_t)(millis() - _mixUntil) < 0) {
    uint32_t left = (_mixUntil - millis() + 999) / 1000UL;
    setWait("mix", left);
    return;
  }
  if ((int32_t)(millis() - _nextEval) < 0) return;
  _nextEval = millis() + 1000;

  if (!pumps.floatOk()) {
    snprintf(_status, sizeof(_status), "Reservoir empty");
    return;
  }

  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    switch (cfg.pump[i].mode) {
      case MODE_SCHEDULED: evalScheduled(i); break;
      case MODE_AUTO:      evalAuto(i);      break;
      default: break;
    }
    if (pumps.queued() > 0) return;   // handle one action per pass
  }

  if (!pumps.busy() && pumps.queued() == 0) {
    // Nothing to dose right now: say what we are waiting on rather than "Idle".
    uint8_t who = 0;
    uint32_t sec = 0;
    if (nextWait(who, sec)) setWait(cfg.pump[who].name, sec);
    else                    snprintf(_status, sizeof(_status), "Idle");
  }
}
