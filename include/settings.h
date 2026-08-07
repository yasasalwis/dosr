#pragma once
#include <Arduino.h>

static const uint8_t NUM_PUMPS = 4;

enum PumpMode : uint8_t {
  MODE_OFF       = 0,   // pump ignored entirely
  MODE_MANUAL    = 1,   // only doses when a human (or MQTT/web) asks
  MODE_SCHEDULED = 2,   // doses every intervalMin minutes
  MODE_AUTO      = 3    // closed loop against a pH or EC setpoint
};

enum AutoTarget : uint8_t {
  TGT_PH_DOWN = 0,      // dose when pH is above setpoint
  TGT_PH_UP   = 1,      // dose when pH is below setpoint
  TGT_EC_UP   = 2       // dose when EC is below setpoint
};

extern const char* const PUMP_MODE_NAMES[4];
extern const char* const AUTO_TARGET_NAMES[3];

struct PumpCfg {
  char     name[17];          // user-facing label, edited from the web UI
  float    mlPerSec;          // calibration: how fast this pump actually moves liquid
  float    doseMl;            // size of one dose
  uint16_t intervalMin;       // MODE_SCHEDULED period
  uint16_t lockoutMin;        // MODE_AUTO minimum gap between doses
  uint8_t  mode;              // PumpMode
  uint8_t  target;            // AutoTarget, only used in MODE_AUTO
  uint8_t  maxDosesPerHour;   // hard rate limit, 0 = unlimited
  uint8_t  reserved;
};

struct Settings {
  uint16_t magic;
  uint8_t  version;
  uint8_t  reserved0;

  PumpCfg  pump[NUM_PUMPS];

  // Closed-loop targets
  float    phSetpoint;        // e.g. 6.0
  float    phDeadband;        // e.g. 0.3 -> acts outside 5.7..6.3
  float    ecSetpoint;        // uS/cm
  float    ecDeadband;        // uS/cm

  // Sensor calibration
  float    phVolt4;           // ADS volts measured in pH 4.00 buffer
  float    phVolt7;           // ADS volts measured in pH 7.00 buffer
  float    ecCalFactor;       // uS/cm per volt
  float    tempC;             // manual solution temperature for EC compensation

  // Timing
  uint16_t settleSec;         // wait after a pump stops before trusting sensors
  uint16_t mixWaitSec;        // wait after a dose before the loop may act again

  // Reservoir float switch
  bool     floatEnabled;
  bool     floatActiveLow;

  // Network
  char     wifiSsid[33];
  char     wifiPass[65];
  char     mqttHost[65];
  uint16_t mqttPort;
  char     mqttUser[33];
  char     mqttPass[65];
  char     deviceName[17];
};

extern Settings cfg;

void settingsBegin();          // load from NVS, or write defaults on first boot
void settingsDefaults();       // reset RAM copy to factory values
void settingsMarkDirty();      // schedule a save (debounced, saves NVS wear)
void settingsTick();           // call from loop(); performs the deferred write
void settingsSaveNow();        // force an immediate write
bool settingsPendingSave();
