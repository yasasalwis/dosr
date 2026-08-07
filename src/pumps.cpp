#include "pumps.h"

Pumps pumps;

const char* doseResultText(DoseResult r) {
  switch (r) {
    case DOSE_QUEUED:              return "queued";
    case DOSE_REJECT_MODE:         return "pump is off";
    case DOSE_REJECT_RATE:         return "hourly limit reached";
    case DOSE_REJECT_QUEUE_FULL:   return "queue full";
    case DOSE_REJECT_CALIBRATION:  return "pump not calibrated";
    case DOSE_REJECT_FLOAT:        return "reservoir empty";
    case DOSE_REJECT_LOCKOUT:      return "still in lockout";
    default:                       return "invalid request";
  }
}

void Pumps::begin() {
  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    // Core 3.x requires pinMode() before digitalWrite(). The GPIO output
    // register reads 0 after reset, so enabling the output drives the gate
    // low immediately. The 100k gate pull-down is what actually guarantees
    // the pump stays off during reset and boot, before any code runs.
    pinMode(PUMP_PINS[i], OUTPUT);
    digitalWrite(PUMP_PINS[i], LOW);
  }
  pinMode(PIN_FLOAT, INPUT_PULLUP);
  _lastStopMs = millis();
}

bool Pumps::floatOk() const {
  if (!cfg.floatEnabled) return true;
  int v = digitalRead(PIN_FLOAT);
  return cfg.floatActiveLow ? (v == LOW) : (v == HIGH);
}

uint8_t Pumps::dosesLastHour(uint8_t i) {
  if (i >= NUM_PUMPS) return 0;
  uint32_t now = millis();
  uint8_t n = 0;
  for (uint8_t k = 0; k < DOSE_HISTORY_LEN; k++) {
    uint32_t t = _history[i][k];
    if (t != 0 && now - t < 3600000UL) n++;
  }
  return n;
}

void Pumps::recordDose(uint8_t idx) {
  _history[idx][_histIdx[idx]] = millis();
  _histIdx[idx] = (_histIdx[idx] + 1) % DOSE_HISTORY_LEN;
  _lastDoseMs[idx] = millis();
}

bool Pumps::push(const Job& j) {
  if (_qCount >= DOSE_QUEUE_LEN) return false;
  _queue[_qTail] = j;
  _qTail = (_qTail + 1) % DOSE_QUEUE_LEN;
  _qCount++;
  return true;
}

DoseResult Pumps::request(uint8_t idx, float ml, const char* source) {
  if (idx >= NUM_PUMPS || !(ml > 0) || ml > PUMP_MAX_DOSE_ML) return DOSE_REJECT_BADARG;
  if (cfg.pump[idx].mode == MODE_OFF) return DOSE_REJECT_MODE;
  if (cfg.pump[idx].mlPerSec < PUMP_MIN_ML_PER_S) return DOSE_REJECT_CALIBRATION;
  if (!floatOk()) return DOSE_REJECT_FLOAT;

  uint8_t limit = cfg.pump[idx].maxDosesPerHour;
  if (limit > 0 && dosesLastHour(idx) >= limit) return DOSE_REJECT_RATE;

  uint32_t ms = (uint32_t)((ml / cfg.pump[idx].mlPerSec) * 1000.0f);
  if (ms == 0) return DOSE_REJECT_BADARG;
  if (ms > PUMP_MAX_RUN_MS) ms = PUMP_MAX_RUN_MS;

  Job j { idx, ms, ml };
  if (!push(j)) return DOSE_REJECT_QUEUE_FULL;

  Serial.printf("[pump] %u dose %.2f ml (%lu ms) from %s\n",
                idx + 1, ml, (unsigned long)ms, source ? source : "?");
  return DOSE_QUEUED;
}

DoseResult Pumps::requestTimed(uint8_t idx, uint32_t ms, const char* source) {
  if (idx >= NUM_PUMPS || ms == 0) return DOSE_REJECT_BADARG;
  if (ms > PUMP_MAX_RUN_MS) ms = PUMP_MAX_RUN_MS;
  if (!floatOk()) return DOSE_REJECT_FLOAT;

  Job j { idx, ms, 0.0f };
  if (!push(j)) return DOSE_REJECT_QUEUE_FULL;

  Serial.printf("[pump] %u timed run %lu ms from %s\n",
                idx + 1, (unsigned long)ms, source ? source : "?");
  return DOSE_QUEUED;
}

void Pumps::startNext() {
  if (_running >= 0 || _qCount == 0) return;
  Job j = _queue[_qHead];
  _qHead = (_qHead + 1) % DOSE_QUEUE_LEN;
  _qCount--;

  _running  = j.idx;
  _runMl    = j.ml;
  _runUntil = millis() + j.ms;
  digitalWrite(PUMP_PINS[j.idx], HIGH);
  recordDose(j.idx);
}

void Pumps::stopCurrent() {
  if (_running < 0) return;
  digitalWrite(PUMP_PINS[_running], LOW);
  _totalMl[_running] += _runMl;
  Serial.printf("[pump] %u stopped\n", _running + 1);
  _running    = -1;
  _runMl      = 0;
  _lastStopMs = millis();
}

void Pumps::stopAll() {
  stopCurrent();
  for (uint8_t i = 0; i < NUM_PUMPS; i++) digitalWrite(PUMP_PINS[i], LOW);
  _qHead = _qTail = _qCount = 0;
  _lastStopMs = millis();
}

uint32_t Pumps::runRemainingMs() const {
  if (_running < 0) return 0;
  uint32_t now = millis();
  return (_runUntil > now) ? (_runUntil - now) : 0;
}

void Pumps::tick() {
  // A float switch going empty mid-dose aborts everything.
  if (_running >= 0 && !floatOk()) {
    Serial.println(F("[pump] float switch tripped, aborting"));
    stopAll();
    return;
  }

  if (_running >= 0) {
    if ((int32_t)(millis() - _runUntil) >= 0) stopCurrent();
    return;
  }

  // Small gap between queued doses so the supply rail recovers.
  if (millis() - _lastStopMs < 500) return;
  startNext();
}