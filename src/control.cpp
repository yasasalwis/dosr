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

void Control::evalAuto(uint8_t i) {
  const PumpCfg& p = cfg.pump[i];
  const Reading& r = sensors.last();

  if (!r.valid) return;
  if (millis() - r.ts > 30000UL) return;             // reading is stale
  if (millis() - pumps.lastDoseMs(i) < (uint32_t)p.lockoutMin * 60000UL) return;

  bool act = false;
  switch (p.target) {
    case TGT_PH_DOWN:
      if (!isnan(r.ph) && r.ph > cfg.phSetpoint + cfg.phDeadband) act = true;
      break;
    case TGT_PH_UP:
      if (!isnan(r.ph) && r.ph < cfg.phSetpoint - cfg.phDeadband) act = true;
      break;
    case TGT_EC_UP:
      if (!isnan(r.ec) && r.ec < cfg.ecSetpoint - cfg.ecDeadband) act = true;
      break;
  }
  if (!act) return;

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

void Control::tick() {
  if (_paused) return;
  if (pumps.busy() || pumps.queued() > 0) return;
  if ((int32_t)(millis() - _mixUntil) < 0) {
    uint32_t left = (_mixUntil - millis()) / 1000UL;
    snprintf(_status, sizeof(_status), "Mixing, %lus", (unsigned long)left);
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

  if (!pumps.busy() && pumps.queued() == 0 &&
      strncmp(_status, "Mixing", 6) != 0) {
    snprintf(_status, sizeof(_status), "Idle");
  }
}
