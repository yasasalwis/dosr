#include "sensors.h"
#include "config.h"
#include "i2c.h"
#include "settings.h"
#include "pumps.h"
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

Sensors sensors;

static Adafruit_ADS1115 ads;
static const uint32_t READ_PERIOD_MS = 1000;

bool Sensors::begin() {
  // Probe first. Without this, a missing ADS1115 produces a wall of
  // i2cWrite() errors from inside the library.
  if (!i2cProbe(ADDR_ADS1115)) {
    Serial.println(F("[adc] no ADS1115 at 0x48"));
    _present = false;
    return false;
  }
  _present = ads.begin(ADDR_ADS1115, &Wire);
  if (_present) {
    // ADS1115 now runs at 3.3 V with a 2:1 divider on each sensor output,
    // so the widest useful range is +-4.096 V. That also doubles the
    // resolution compared with the old +-6.144 V setting.
    ads.setGain(GAIN_ONE);
    ads.setDataRate(RATE_ADS1115_128SPS);
    Serial.println(F("[adc] ADS1115 ready"));
  } else {
    Serial.println(F("[adc] ADS1115 not found"));
  }
  return _present;
}

static int cmpFloat(const void* a, const void* b) {
  float fa = *(const float*)a, fb = *(const float*)b;
  return (fa > fb) - (fa < fb);
}

float Sensors::readChannelVolts(uint8_t ch, uint8_t samples) {
  if (!_present) return NAN;
  if (samples > 15) samples = 15;

  float buf[15];
  for (uint8_t i = 0; i < samples; i++) {
    int16_t raw = ads.readADC_SingleEnded(ch);
    buf[i] = ads.computeVolts(raw);
    delay(2);
  }
  qsort(buf, samples, sizeof(float), cmpFloat);
  // Multiply back through the divider to recover the real sensor voltage,
  // so all calibration figures stay in the sensor's own 0-5 V domain.
  return buf[samples / 2] * ADC_DIVIDER_RATIO;   // median rejects switching spikes
}

bool Sensors::settling() const {
  uint16_t s = cfg.settleSec < SETTLE_MIN_SEC ? SETTLE_MIN_SEC : cfg.settleSec;
  return pumps.busy() || (millis() - pumps.lastStopMs() < (uint32_t)s * 1000UL);
}

float Sensors::phFromVolts(float v) const {
  float span = cfg.phVolt7 - cfg.phVolt4;
  if (isnan(v) || fabsf(span) < 0.02f) return NAN;   // uncalibrated or shorted
  float slope = (7.0f - 4.0f) / span;
  float ph = 4.0f + (v - cfg.phVolt4) * slope;
  if (ph < -1.0f || ph > 15.0f) return NAN;
  return ph;
}

float Sensors::ecFromVolts(float v) const {
  if (isnan(v)) return NAN;
  float raw = v * cfg.ecCalFactor;
  float comp = 1.0f + 0.02f * (cfg.tempC - 25.0f);   // ~2 %/C, the usual approximation
  if (comp < 0.5f) comp = 0.5f;
  float ec = raw / comp;
  if (ec < 0) ec = 0;
  if (ec > 20000.0f) return NAN;
  return ec;
}

void Sensors::tick() {
  if (!_present) return;
  if ((int32_t)(millis() - _nextRead) < 0) return;
  _nextRead = millis() + READ_PERIOD_MS;

  // Never sample while a motor is switching - the ground bounce lands straight
  // on the probe. Keep the last good value and mark it stale instead.
  if (settling()) {
    _last.valid = false;
    return;
  }

  float pv = readChannelVolts(ADS_CH_PH);
  float ev = readChannelVolts(ADS_CH_EC);

  _last.phVolt = pv;
  _last.ecVolt = ev;
  _last.ph     = phFromVolts(pv);
  _last.ec     = ecFromVolts(ev);
  _last.ts     = millis();
  _last.valid  = !isnan(_last.ph) || !isnan(_last.ec);
}

bool Sensors::calibratePh(float knownPh) {
  if (!_present || settling()) return false;
  float v = readChannelVolts(ADS_CH_PH, 15);
  if (isnan(v)) return false;

  if (fabsf(knownPh - 4.0f) < 0.5f)      cfg.phVolt4 = v;
  else if (fabsf(knownPh - 7.0f) < 0.5f) cfg.phVolt7 = v;
  else return false;

  settingsMarkDirty();
  Serial.printf("[cal] pH %.1f -> %.4f V\n", knownPh, v);
  return true;
}

bool Sensors::calibrateEc(float knownUsPerCm) {
  if (!_present || settling() || !(knownUsPerCm > 0)) return false;
  float v = readChannelVolts(ADS_CH_EC, 15);
  if (isnan(v) || v < 0.02f) return false;   // probe probably not in solution

  float comp = 1.0f + 0.02f * (cfg.tempC - 25.0f);
  cfg.ecCalFactor = (knownUsPerCm * comp) / v;
  settingsMarkDirty();
  Serial.printf("[cal] EC %.0f uS/cm at %.4f V -> factor %.1f\n",
                knownUsPerCm, v, cfg.ecCalFactor);
  return true;
}
