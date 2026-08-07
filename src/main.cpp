#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "i2c.h"
#include "settings.h"
#include "pumps.h"
#include "sensors.h"
#include "control.h"
#include "ui.h"
#include "net.h"

// 15 s, not 8. MQTT and WiFi calls can block for several seconds on a bad
// network; the watchdog is here to catch a genuine hang, not slow I/O.
static const uint32_t WDT_TIMEOUT_S = 15;

static const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic / exception";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT - check your supply";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default:               return "unknown";
  }
}

bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static uint8_t i2cScan() {
  Serial.println(F("[i2c] scanning..."));
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (i2cProbe(a)) {
      Serial.printf("[i2c]   device at 0x%02X%s\n", a,
                    a == ADDR_OLED    ? "  (OLED)" :
                    a == ADDR_ADS1115 ? "  (ADS1115)" : "");
      found++;
    }
  }
  if (found == 0) {
    Serial.println(F("[i2c]   nothing found."));
    Serial.println(F("[i2c]   Check: SDA=D4 SCL=D5, both idle high at 3.3V,"));
    Serial.println(F("[i2c]   pull-ups present, devices powered from 3V3."));
  }
  Serial.printf("[i2c] %u device(s)\n", found);
  return found;
}

void setup() {
  // Gates low first, before anything can go wrong.
  pumps.begin();

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) { delay(10); }
  Serial.println();
  Serial.println(F("=== Mini Feeder " FW_VERSION " ==="));

  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[boot] reset reason: %d (%s)\n", (int)rr, resetReasonText(rr));
  if (rr == ESP_RST_BROWNOUT) {
    Serial.println(F("[boot] Supply sagged. Do not run USB and the buck"));
    Serial.println(F("[boot] converter into the 5V pin at the same time."));
  }

  settingsBegin();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  delay(50);                 // let the bus and any shifter settle
  i2cScan();

  // Both of these probe their address first and return false if absent, so a
  // missing device logs one line instead of flooding the console with
  // i2cWrite() failures.
  if (!ui.begin())      Serial.println(F("[boot] no display, running headless"));
  if (!sensors.begin()) Serial.println(F("[boot] no ADC, sensors disabled"));

  control.begin();
  netBegin();

  esp_task_wdt_config_t wdt = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(NULL);

  Serial.println(F("[boot] ready"));
}

void loop() {
  esp_task_wdt_reset();

  pumps.tick();      // stopping a running pump on time matters most
  sensors.tick();
  control.tick();
  ui.tick();
  netTick();
  settingsTick();

  delay(1);
}
