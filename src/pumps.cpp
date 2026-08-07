#include "pumps.h"
#include "i2c.h"
#include <Wire.h>
#include <Adafruit_MCP23X17.h>

Pumps pumps;

static Adafruit_MCP23X17 mcp;

// A gate write that will not take is the one failure this firmware cannot
// talk its way out of: the motor is running and the only path to its gate is
// a bus that is not answering. After this long we stop trying and reset. The
// MCP23017 powers up with every pin a high-impedance input, so the 100k gate
// pull-downs cut the pumps the moment the reset line drops.
static const uint32_t GATE_FAIL_REBOOT_MS = 2000;

static const uint32_t FLOAT_READ_MS = 50;

const char* doseResultText(DoseResult r) {
  switch (r) {
    case DOSE_QUEUED:              return "queued";
    case DOSE_REJECT_MODE:         return "pump is off";
    case DOSE_REJECT_RATE:         return "hourly limit reached";
    case DOSE_REJECT_QUEUE_FULL:   return "queue full";
    case DOSE_REJECT_CALIBRATION:  return "pump not calibrated";
    case DOSE_REJECT_FLOAT:        return "reservoir empty";
    case DOSE_REJECT_LOCKOUT:      return "still in lockout";
    case DOSE_REJECT_NO_DRIVER:    return "pump driver offline";
    default:                       return "invalid request";
  }
}

// Writes port A and reads it back. The Adafruit library returns nothing from
// writeGPIO(), so a read-back is the only way to know the transaction landed.
bool Pumps::writeGates(uint8_t mask) {
  if (!_driverOk) return false;
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    mcp.writeGPIO(mask, 0);                 // port 0 = GPIOA
    if (mcp.readGPIO(0) == mask) {
      _gates = mask;
      return true;
    }
  }
  // tick() retries this every pass, so a dead bus would otherwise bury the
  // console in identical lines and starve everything else on the loop.
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 500) {
    lastLog = millis();
    Serial.printf("[pump] gate write 0x%02X did not take\n", mask);
  }
  return false;
}

void Pumps::begin() {
  // Unlike the direct-GPIO version this cannot run before Wire.begin() -
  // main.cpp starts I2C first. Between reset and this point the expander sits
  // in its power-on state, every pin a high-impedance input, and the gate
  // pull-downs are what hold the pumps off. That is the whole reason this
  // design uses an MCP23017 and not a PCF8574, whose outputs power up high.
  if (!i2cProbe(ADDR_MCP23017) || !mcp.begin_I2C(ADDR_MCP23017, &Wire)) {
    _driverOk = false;
    Serial.println(F("[pump] no MCP23017 at 0x20 - dosing disabled"));
    Serial.println(F("[pump] Check: SDA=D4 SCL=D5, A0/A1/A2 strapped to GND."));
    return;
  }
  _driverOk = true;

  // Clear the output latches before flipping direction, so the pins drive low
  // the instant they become outputs rather than picking up whatever was there.
  mcp.writeGPIO(0x00, 0);
  for (uint8_t i = 0; i < NUM_PUMPS; i++) mcp.pinMode(MCP_PUMP_PINS[i], OUTPUT);
  writeGates(0);

  mcp.pinMode(MCP_PIN_FLOAT, INPUT_PULLUP);
  readFloat();

  _lastStopMs = millis();
  Serial.println(F("[pump] MCP23017 ready, gates low"));
}

void Pumps::readFloat() {
  if (!_driverOk) { _floatOk = false; return; }
  int v = mcp.digitalRead(MCP_PIN_FLOAT);
  _floatOk = cfg.floatActiveLow ? (v == LOW) : (v == HIGH);
}

bool Pumps::floatOk() const {
  if (!cfg.floatEnabled) return true;
  return _floatOk;
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
  if (!_driverOk) return DOSE_REJECT_NO_DRIVER;
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
  if (!_driverOk) return DOSE_REJECT_NO_DRIVER;
  if (ms > PUMP_MAX_RUN_MS) ms = PUMP_MAX_RUN_MS;
  if (!floatOk()) return DOSE_REJECT_FLOAT;

  Job j { idx, ms, 0.0f };
  if (!push(j)) return DOSE_REJECT_QUEUE_FULL;

  Serial.printf("[pump] %u timed run %lu ms from %s\n",
                idx + 1, (unsigned long)ms, source ? source : "?");
  return DOSE_QUEUED;
}

void Pumps::startNext() {
  if (_running >= 0 || _qCount == 0 || !_driverOk) return;
  Job j = _queue[_qHead];

  // Only consume the job once the gate is actually high. If the bus fails
  // here nothing has started, so leaving the job queued is safe.
  if (!writeGates((uint8_t)(_gates | (1u << j.idx)))) return;

  _qHead = (_qHead + 1) % DOSE_QUEUE_LEN;
  _qCount--;
  _running  = j.idx;
  _runMl    = j.ml;
  _runUntil = millis() + j.ms;
  recordDose(j.idx);
}

void Pumps::stopCurrent() {
  if (_running < 0) return;

  if (!writeGates((uint8_t)(_gates & ~(1u << _running)))) {
    if (_stopFailSince == 0) _stopFailSince = millis();
    if (millis() - _stopFailSince > GATE_FAIL_REBOOT_MS) {
      Serial.println(F("[pump] gate stuck high and the bus is dead - resetting"));
      Serial.flush();
      ESP.restart();
    }
    return;   // _running stays set, so tick() retries on the next pass
  }

  _stopFailSince = 0;
  _totalMl[_running] += _runMl;
  Serial.printf("[pump] %d stopped\n", _running + 1);
  _running    = -1;
  _runMl      = 0;
  _lastStopMs = millis();
}

void Pumps::stopAll() {
  _qHead = _qTail = _qCount = 0;
  stopCurrent();
  writeGates(0);              // belt and braces, whatever the shadow says
  _lastStopMs = millis();
}

uint32_t Pumps::runRemainingMs() const {
  if (_running < 0) return 0;
  uint32_t now = millis();
  return (_runUntil > now) ? (_runUntil - now) : 0;
}

void Pumps::tick() {
  if (!_driverOk) return;

  // The switch is behind the expander now, so it is polled rather than read
  // on demand. 50 ms of latency against a dose measured in seconds.
  if (cfg.floatEnabled && (int32_t)(millis() - _nextFloatRead) >= 0) {
    _nextFloatRead = millis() + FLOAT_READ_MS;
    readFloat();
  }

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
