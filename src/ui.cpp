#include "ui.h"
#include "config.h"
#include "i2c.h"
#include "settings.h"
#include "pumps.h"
#include "sensors.h"
#include "control.h"
#include "net.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

Ui ui;

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ---------------------------------------------------------------------------
// Encoder: quadrature decoded in an ISR, four transitions per detent.
// ---------------------------------------------------------------------------
static volatile int32_t  encPos    = 0;
static volatile int8_t   encAccum  = 0;
static volatile uint8_t  encState  = 0;
static volatile uint32_t encIsrHits = 0;   // storm detector
static bool encAttached = false;

static const int8_t ENC_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

static void IRAM_ATTR encIsr() {
  uint8_t s = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  encState = (uint8_t)(((encState << 2) | s) & 0x0F);
  encAccum = (int8_t)(encAccum + ENC_TABLE[encState]);
  if (encAccum >= 4)       { encPos++; encAccum = 0; }
  else if (encAccum <= -4) { encPos--; encAccum = 0; }
  encIsrHits++;
}

// A floating encoder input generates tens of thousands of interrupts per
// second, which starves USB and WiFi and eventually trips the watchdog.
// If that happens, drop the interrupts rather than let the board lock up.
static void encStormCheck() {
  if (!encAttached) return;
  static uint32_t window = 0;
  if (millis() - window < 1000) return;
  window = millis();

  noInterrupts();
  uint32_t hits = encIsrHits;
  encIsrHits = 0;
  interrupts();

  if (hits > ENC_ISR_STORM_PER_SEC) {
    detachInterrupt(digitalPinToInterrupt(PIN_ENC_A));
    detachInterrupt(digitalPinToInterrupt(PIN_ENC_B));
    encAttached = false;
    Serial.printf("[enc] %lu interrupts in 1 s - encoder disabled.\n",
                  (unsigned long)hits);
    Serial.println(F("[enc] Pins D1/D2 are floating or noisy. Check wiring,"));
    Serial.println(F("[enc] or set ENCODER_ENABLED 0 in config.h."));
  }
}

int32_t uiEncoderPosition() { return encPos; }

static int32_t encLast = 0;
static int8_t takeEncoderDelta() {
  noInterrupts();
  int32_t now = encPos;
  interrupts();
  int32_t d = now - encLast;
  encLast = now;
  if (d > 8) d = 8;
  if (d < -8) d = -8;
  return (int8_t)d;
}

// Button: short press = select, long press = back.
static bool     btnStable = true, btnPrev = true;
static uint32_t btnChanged = 0, btnDownAt = 0;
static bool     btnLongFired = false;
static const uint32_t DEBOUNCE_MS = 25, LONG_MS = 600;

enum BtnEvent { BTN_NONE, BTN_SHORT, BTN_LONG };

static BtnEvent readButton() {
  bool raw = digitalRead(PIN_ENC_SW);
  if (raw != btnPrev) { btnPrev = raw; btnChanged = millis(); }
  if (millis() - btnChanged < DEBOUNCE_MS) return BTN_NONE;

  if (raw != btnStable) {
    btnStable = raw;
    if (!raw) { btnDownAt = millis(); btnLongFired = false; }
    else if (!btnLongFired) return BTN_SHORT;
  }
  if (!btnStable && !btnLongFired && millis() - btnDownAt > LONG_MS) {
    btnLongFired = true;
    return BTN_LONG;
  }
  return BTN_NONE;
}

// ---------------------------------------------------------------------------
// Menu model
// ---------------------------------------------------------------------------
enum Screen : uint8_t {
  SCR_HOME, SCR_MAIN, SCR_PUMPLIST, SCR_PUMP, SCR_CALIB,
  SCR_SENSORS, SCR_CONTROL, SCR_NETWORK, SCR_SYSTEM
};

enum Kind : uint8_t {
  K_SUBMENU, K_FLOAT, K_U16, K_U8, K_ENUM, K_BOOL, K_ACTION, K_INFO
};

enum Action : uint8_t {
  A_NONE, A_RUN_NOW, A_PRIME, A_CAL_RUN, A_STOP_ALL, A_PAUSE,
  A_CAL_PH4, A_CAL_PH7, A_CAL_EC, A_SAVE, A_FACTORY, A_REBOOT, A_WIFI_RETRY
};

struct Item {
  char     label[19];
  Kind     kind;
  void*    ptr;
  float    mn, mx, step;
  const char* const* names;
  uint8_t  nNames;
  uint8_t  arg;          // target screen for K_SUBMENU, Action for K_ACTION
  char     info[14];     // pre-rendered text for K_INFO
};

static const uint8_t MAX_ITEMS = 14;
static Item   items[MAX_ITEMS];
static uint8_t nItems = 0;

static Screen  scrStack[6] = { SCR_HOME };
static uint8_t selStack[6] = { 0 };
static uint8_t depth = 0;

static uint8_t curPump = 0;
static bool    editing = false;
static uint8_t topRow  = 0;

static char     toastMsg[22] = "";
static uint32_t toastUntil   = 0;

// Scratch values that are not part of persisted settings
static float calMeasuredMl[NUM_PUMPS] = { 0, 0, 0, 0 };
static float ecCalStandard = 1413.0f;
static const uint16_t CAL_RUN_SEC = 20;

static const char* const BOOL_NAMES[2] = { "No", "Yes" };

// ---------------------------------------------------------------------------
// Item builders
// ---------------------------------------------------------------------------
static void addItem(const char* label, Kind k, void* ptr,
                    float mn = 0, float mx = 0, float step = 0,
                    const char* const* names = nullptr, uint8_t nNames = 0,
                    uint8_t arg = 0, const char* info = nullptr) {
  if (nItems >= MAX_ITEMS) return;
  Item& it = items[nItems++];
  memset(&it, 0, sizeof(it));
  strlcpy(it.label, label, sizeof(it.label));
  it.kind = k; it.ptr = ptr;
  it.mn = mn; it.mx = mx; it.step = step;
  it.names = names; it.nNames = nNames; it.arg = arg;
  if (info) strlcpy(it.info, info, sizeof(it.info));
}

static void buildMain() {
  addItem("Pumps",    K_SUBMENU, nullptr, 0,0,0, nullptr,0, SCR_PUMPLIST);
  addItem("Sensors",  K_SUBMENU, nullptr, 0,0,0, nullptr,0, SCR_SENSORS);
  addItem("Control",  K_SUBMENU, nullptr, 0,0,0, nullptr,0, SCR_CONTROL);
  addItem("Network",  K_SUBMENU, nullptr, 0,0,0, nullptr,0, SCR_NETWORK);
  addItem("System",   K_SUBMENU, nullptr, 0,0,0, nullptr,0, SCR_SYSTEM);
  addItem(control.paused() ? "Resume dosing" : "Pause dosing",
          K_ACTION, nullptr, 0,0,0, nullptr,0, A_PAUSE);
  addItem("Stop all now", K_ACTION, nullptr, 0,0,0, nullptr,0, A_STOP_ALL);
}

static void buildPumpList() {
  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    addItem(cfg.pump[i].name, K_SUBMENU, nullptr, 0,0,0, nullptr,0, SCR_PUMP);
  }
}

static void buildPump() {
  PumpCfg& p = cfg.pump[curPump];
  addItem("Mode",        K_ENUM,  &p.mode, 0,3,1, PUMP_MODE_NAMES, 4);
  addItem("Dose size ml", K_FLOAT, &p.doseMl, 0.1f, PUMP_MAX_DOSE_ML, 0.5f);
  if (p.mode == MODE_SCHEDULED)
    addItem("Every (min)", K_U16, &p.intervalMin, 1, 10080, 5);
  if (p.mode == MODE_AUTO) {
    addItem("Corrects",    K_ENUM, &p.target, 0,2,1, AUTO_TARGET_NAMES, 3);
    addItem("Lockout min", K_U16,  &p.lockoutMin, 1, 720, 5);
  }
  addItem("Max doses/hr", K_U8,   &p.maxDosesPerHour, 0, 12, 1);
  addItem("Run one dose", K_ACTION, nullptr, 0,0,0, nullptr,0, A_RUN_NOW);
  addItem("Prime 10s",    K_ACTION, nullptr, 0,0,0, nullptr,0, A_PRIME);
  addItem("Calibrate",    K_SUBMENU, nullptr, 0,0,0, nullptr,0, SCR_CALIB);
}

static void buildCalib() {
  char buf[14];
  snprintf(buf, sizeof(buf), "%.3f", cfg.pump[curPump].mlPerSec);
  addItem("Now ml/s",     K_INFO,  nullptr, 0,0,0, nullptr,0,0, buf);
  addItem("1. Run 20s",   K_ACTION, nullptr, 0,0,0, nullptr,0, A_CAL_RUN);
  addItem("2. Measured ml", K_FLOAT, &calMeasuredMl[curPump], 0, 500, 0.5f);
  addItem("Set ml/s",     K_FLOAT, &cfg.pump[curPump].mlPerSec, 0.05f, 20.0f, 0.01f);
}

static void buildSensors() {
  char buf[14];
  const Reading& r = sensors.last();

  if (isnan(r.ph)) strlcpy(buf, "--", sizeof(buf));
  else snprintf(buf, sizeof(buf), "%.2f", r.ph);
  addItem("pH now", K_INFO, nullptr, 0,0,0, nullptr,0,0, buf);

  snprintf(buf, sizeof(buf), "%.3fV", isnan(r.phVolt) ? 0.0f : r.phVolt);
  addItem("pH volts", K_INFO, nullptr, 0,0,0, nullptr,0,0, buf);

  addItem("Cal in pH 4.00", K_ACTION, nullptr, 0,0,0, nullptr,0, A_CAL_PH4);
  addItem("Cal in pH 7.00", K_ACTION, nullptr, 0,0,0, nullptr,0, A_CAL_PH7);

  if (isnan(r.ec)) strlcpy(buf, "--", sizeof(buf));
  else snprintf(buf, sizeof(buf), "%.0f", r.ec);
  addItem("EC uS/cm", K_INFO, nullptr, 0,0,0, nullptr,0,0, buf);

  addItem("EC standard", K_FLOAT, &ecCalStandard, 100, 20000, 10);
  addItem("Cal EC now",  K_ACTION, nullptr, 0,0,0, nullptr,0, A_CAL_EC);
  addItem("Solution C",  K_FLOAT, &cfg.tempC, 5, 45, 0.5f);
  addItem("Settle sec",  K_U16,   &cfg.settleSec, SETTLE_MIN_SEC, 120, 1);
}

static void buildControl() {
  addItem("pH setpoint",  K_FLOAT, &cfg.phSetpoint, 3.0f, 9.0f, 0.05f);
  addItem("pH deadband",  K_FLOAT, &cfg.phDeadband, 0.05f, 2.0f, 0.05f);
  addItem("EC setpoint",  K_FLOAT, &cfg.ecSetpoint, 100, 5000, 25);
  addItem("EC deadband",  K_FLOAT, &cfg.ecDeadband, 10, 1000, 10);
  addItem("Mix wait sec", K_U16,   &cfg.mixWaitSec, 5, 3600, 15);
  addItem("Float switch", K_BOOL,  &cfg.floatEnabled, 0,1,1, BOOL_NAMES, 2);
  addItem("Float act low", K_BOOL, &cfg.floatActiveLow, 0,1,1, BOOL_NAMES, 2);
}

static void buildNetwork() {
  char buf[14];
  strlcpy(buf, netWifiStateText(), sizeof(buf));
  addItem("WiFi",   K_INFO, nullptr, 0,0,0, nullptr,0,0, buf);
  strlcpy(buf, netIpText(), sizeof(buf));
  addItem("IP",     K_INFO, nullptr, 0,0,0, nullptr,0,0, buf);
  strlcpy(buf, netMqttStateText(), sizeof(buf));
  addItem("MQTT",   K_INFO, nullptr, 0,0,0, nullptr,0,0, buf);
  addItem("Reconnect", K_ACTION, nullptr, 0,0,0, nullptr,0, A_WIFI_RETRY);
  addItem("Setup", K_INFO, nullptr, 0,0,0, nullptr,0,0, "web page");
}

static void buildSystem() {
  addItem("Version", K_INFO, nullptr, 0,0,0, nullptr,0,0, FW_VERSION);
  addItem("Save now",     K_ACTION, nullptr, 0,0,0, nullptr,0, A_SAVE);
  addItem("Factory reset", K_ACTION, nullptr, 0,0,0, nullptr,0, A_FACTORY);
  addItem("Reboot",       K_ACTION, nullptr, 0,0,0, nullptr,0, A_REBOOT);
}

static void rebuild() {
  nItems = 0;
  switch (scrStack[depth]) {
    case SCR_MAIN:     buildMain();     break;
    case SCR_PUMPLIST: buildPumpList(); break;
    case SCR_PUMP:     buildPump();     break;
    case SCR_CALIB:    buildCalib();    break;
    case SCR_SENSORS:  buildSensors();  break;
    case SCR_CONTROL:  buildControl();  break;
    case SCR_NETWORK:  buildNetwork();  break;
    case SCR_SYSTEM:   buildSystem();   break;
    default: break;
  }
  if (selStack[depth] >= nItems) selStack[depth] = nItems ? nItems - 1 : 0;
}

static void pushScreen(Screen s) {
  if (depth >= 5) return;
  depth++;
  scrStack[depth] = s;
  selStack[depth] = 0;
  topRow = 0;
  rebuild();
}

static void popScreen() {
  if (depth == 0) return;
  depth--;
  topRow = 0;
  rebuild();
}

// ---------------------------------------------------------------------------
// Value read / write helpers
// ---------------------------------------------------------------------------
static float itemValue(const Item& it) {
  switch (it.kind) {
    case K_FLOAT: return *(float*)it.ptr;
    case K_U16:   return (float)*(uint16_t*)it.ptr;
    case K_U8:
    case K_ENUM:  return (float)*(uint8_t*)it.ptr;
    case K_BOOL:  return *(bool*)it.ptr ? 1.0f : 0.0f;
    default:      return 0;
  }
}

static void itemSet(const Item& it, float v) {
  if (v < it.mn) v = it.mn;
  if (v > it.mx) v = it.mx;
  switch (it.kind) {
    case K_FLOAT: *(float*)it.ptr = v; break;
    case K_U16:   *(uint16_t*)it.ptr = (uint16_t)lroundf(v); break;
    case K_U8:
    case K_ENUM:  *(uint8_t*)it.ptr = (uint8_t)lroundf(v); break;
    case K_BOOL:  *(bool*)it.ptr = (v >= 0.5f); break;
    default: break;
  }
  settingsMarkDirty();
}

static void itemValueText(const Item& it, char* out, size_t n) {
  switch (it.kind) {
    case K_SUBMENU: strlcpy(out, ">", n); break;
    case K_ACTION:  strlcpy(out, "*", n); break;
    case K_INFO:    strlcpy(out, it.info, n); break;
    case K_ENUM:
    case K_BOOL: {
      uint8_t idx = (uint8_t)itemValue(it);
      if (it.names && idx < it.nNames) strlcpy(out, it.names[idx], n);
      else snprintf(out, n, "%u", idx);
      break;
    }
    case K_FLOAT: {
      float v = itemValue(it);
      if (it.step >= 1.0f)       snprintf(out, n, "%.0f", v);
      else if (it.step >= 0.1f)  snprintf(out, n, "%.1f", v);
      else if (it.step >= 0.05f) snprintf(out, n, "%.2f", v);
      else                       snprintf(out, n, "%.3f", v);
      break;
    }
    default:
      snprintf(out, n, "%.0f", itemValue(it));
      break;
  }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void runAction(uint8_t a) {
  switch (a) {
    case A_RUN_NOW: {
      DoseResult r = pumps.request(curPump, cfg.pump[curPump].doseMl, "menu");
      ui.toast(r == DOSE_QUEUED ? "Dosing" : doseResultText(r));
      break;
    }
    case A_PRIME: {
      DoseResult r = pumps.requestTimed(curPump, 10000, "prime");
      ui.toast(r == DOSE_QUEUED ? "Priming 10s" : doseResultText(r));
      break;
    }
    case A_CAL_RUN: {
      DoseResult r = pumps.requestTimed(curPump, CAL_RUN_SEC * 1000UL, "calib");
      ui.toast(r == DOSE_QUEUED ? "Running 20s" : doseResultText(r));
      break;
    }
    case A_STOP_ALL:
      pumps.stopAll();
      ui.toast("All pumps stopped");
      break;
    case A_PAUSE:
      control.setPaused(!control.paused());
      ui.toast(control.paused() ? "Dosing paused" : "Dosing resumed");
      rebuild();
      break;
    case A_CAL_PH4:
      ui.toast(sensors.calibratePh(4.0f) ? "pH 4 stored" : "Not ready");
      rebuild();
      break;
    case A_CAL_PH7:
      ui.toast(sensors.calibratePh(7.0f) ? "pH 7 stored" : "Not ready");
      rebuild();
      break;
    case A_CAL_EC:
      ui.toast(sensors.calibrateEc(ecCalStandard) ? "EC stored" : "Not ready");
      rebuild();
      break;
    case A_SAVE:
      settingsSaveNow();
      ui.toast("Settings saved");
      break;
    case A_FACTORY:
      settingsDefaults();
      settingsSaveNow();
      ui.toast("Reset to defaults");
      rebuild();
      break;
    case A_REBOOT:
      pumps.stopAll();
      settingsSaveNow();
      delay(200);
      ESP.restart();
      break;
    case A_WIFI_RETRY:
      netReconnect();
      ui.toast("Reconnecting");
      break;
    default: break;
  }
}

// A calibration entry gets turned into ml/s the moment it is confirmed.
static void applyCalibrationEntry() {
  if (scrStack[depth] != SCR_CALIB) return;
  float ml = calMeasuredMl[curPump];
  if (ml > 0.1f) {
    cfg.pump[curPump].mlPerSec = ml / (float)CAL_RUN_SEC;
    settingsMarkDirty();
    ui.toast("Calibration saved");
    rebuild();
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawHome() {
  const Reading& r = sensors.last();
  char buf[24];

  oled.setTextSize(2);
  oled.setCursor(0, 2);
  if (isnan(r.ph)) oled.print(F("pH --.-"));
  else { snprintf(buf, sizeof(buf), "pH %.2f", r.ph); oled.print(buf); }

  oled.setCursor(0, 22);
  if (isnan(r.ec)) oled.print(F("EC ----"));
  else { snprintf(buf, sizeof(buf), "EC %.0f", r.ec); oled.print(buf); }

  oled.setTextSize(1);
  oled.setCursor(104, 28);
  oled.print(F("uS"));

  if (!r.valid) {
    oled.setCursor(104, 4);
    oled.print(F("~"));
  }

  oled.drawFastHLine(0, 42, 128, SSD1306_WHITE);

  oled.setCursor(0, 46);
  if (pumps.busy()) {
    int8_t p = pumps.running();
    snprintf(buf, sizeof(buf), "%.16s %lus",
             cfg.pump[p].name, (unsigned long)((pumps.runRemainingMs() + 999) / 1000));
    oled.print(buf);
  } else {
    oled.print(control.statusText());
  }

  oled.setCursor(0, 56);
  snprintf(buf, sizeof(buf), "%-14.14s %s", netIpText(),
           settingsPendingSave() ? "*" : " ");
  oled.print(buf);
}

static void drawMenu() {
  const char* title = "Menu";
  switch (scrStack[depth]) {
    case SCR_PUMPLIST: title = "Pumps"; break;
    case SCR_PUMP:     title = cfg.pump[curPump].name; break;
    case SCR_CALIB:    title = "Calibrate pump"; break;
    case SCR_SENSORS:  title = "Sensors"; break;
    case SCR_CONTROL:  title = "Control"; break;
    case SCR_NETWORK:  title = "Network"; break;
    case SCR_SYSTEM:   title = "System"; break;
    default: break;
  }

  oled.fillRect(0, 0, 128, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setCursor(2, 2);
  oled.print(title);
  oled.setTextColor(SSD1306_WHITE);

  const uint8_t ROWS = 5, ROW_H = 10, Y0 = 13;
  uint8_t sel = selStack[depth];
  if (sel < topRow) topRow = sel;
  if (sel >= topRow + ROWS) topRow = sel - ROWS + 1;

  char val[16];
  for (uint8_t i = 0; i < ROWS && (topRow + i) < nItems; i++) {
    uint8_t idx = topRow + i;
    uint8_t y = Y0 + i * ROW_H;
    bool on = (idx == sel);

    if (on) {
      oled.fillRect(0, y - 1, 128, ROW_H, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else {
      oled.setTextColor(SSD1306_WHITE);
    }

    oled.setCursor(2, y);
    oled.print(items[idx].label);

    itemValueText(items[idx], val, sizeof(val));
    if (val[0]) {
      uint8_t w = (uint8_t)(strlen(val) * 6);
      uint8_t x = (uint8_t)(126 - w);
      if (on && editing) {
        oled.fillRect(x - 2, y - 1, w + 4, ROW_H, SSD1306_BLACK);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(x, y);
        oled.print(val);
        oled.setTextColor(SSD1306_BLACK);
      } else {
        oled.setCursor(x, y);
        oled.print(val);
      }
    }
  }
  oled.setTextColor(SSD1306_WHITE);

  if (nItems > ROWS) {
    uint8_t barH = (uint8_t)max(4, (int)(51 * ROWS / nItems));
    uint8_t barY = (uint8_t)(13 + (51 - barH) * topRow / (nItems - ROWS));
    oled.drawFastVLine(127, 13, 51, SSD1306_WHITE);
    oled.fillRect(126, barY, 2, barH, SSD1306_WHITE);
  }
}

static void drawToast() {
  if (millis() > toastUntil) return;
  uint8_t w = (uint8_t)(strlen(toastMsg) * 6 + 8);
  if (w > 128) w = 128;
  uint8_t x = (uint8_t)((128 - w) / 2);
  oled.fillRect(x, 26, w, 13, SSD1306_BLACK);
  oled.drawRect(x, 26, w, 13, SSD1306_WHITE);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(x + 4, 30);
  oled.print(toastMsg);
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void handleRotate(int8_t d) {
  if (scrStack[depth] == SCR_HOME) return;
  if (nItems == 0) return;

  if (editing) {
    Item& it = items[selStack[depth]];
    float v = itemValue(it) + d * it.step;
    itemSet(it, v);
    // Changing a pump mode changes which rows exist below it.
    if (it.kind == K_ENUM && scrStack[depth] == SCR_PUMP && it.ptr == &cfg.pump[curPump].mode) {
      uint8_t keep = selStack[depth];
      rebuild();
      selStack[depth] = keep < nItems ? keep : nItems - 1;
    }
    return;
  }

  int16_t s = (int16_t)selStack[depth] + d;
  if (s < 0) s = 0;
  if (s >= (int16_t)nItems) s = nItems - 1;
  selStack[depth] = (uint8_t)s;
}

static void handleShort() {
  if (scrStack[depth] == SCR_HOME) { pushScreen(SCR_MAIN); return; }
  if (nItems == 0) { popScreen(); return; }

  Item& it = items[selStack[depth]];

  if (editing) {
    editing = false;
    if (it.ptr == &calMeasuredMl[curPump]) applyCalibrationEntry();
    return;
  }

  switch (it.kind) {
    case K_SUBMENU:
      if (scrStack[depth] == SCR_PUMPLIST) curPump = selStack[depth];
      pushScreen((Screen)it.arg);
      break;
    case K_ACTION:
      runAction(it.arg);
      break;
    case K_INFO:
      break;
    default:
      editing = true;
      break;
  }
}

static void handleLong() {
  if (editing) { editing = false; return; }
  if (depth == 0) return;
  popScreen();
  if (depth == 0) topRow = 0;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
bool Ui::begin() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  encState = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));

#if ENCODER_ENABLED
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encIsr, CHANGE);
  encAttached = true;
#else
  Serial.println(F("[enc] encoder disabled in config.h"));
#endif

  // Probe the address before handing over to the library. Adafruit's begin()
  // does not check whether its init writes were acknowledged, so a missing
  // display otherwise floods the console with i2cWrite() errors and still
  // reports success.
  if (!i2cProbe(ADDR_OLED)) {
    Serial.println(F("[oled] no display at 0x3C"));
    _present = false;
    return false;
  }
  _present = oled.begin(SSD1306_SWITCHCAPVCC, ADDR_OLED);
  if (!_present) {
    Serial.println(F("[oled] found at 0x3C but init failed"));
    return false;
  }
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println(F("Mini Feeder"));
  oled.println(FW_VERSION);
  oled.display();
  return true;
}

void Ui::toast(const char* msg) {
  strlcpy(toastMsg, msg, sizeof(toastMsg));
  toastUntil = millis() + 1800;
}

void Ui::tick() {
  encStormCheck();

  int8_t d = takeEncoderDelta();
  if (d) handleRotate(d);

  BtnEvent e = readButton();
  if (e == BTN_SHORT) handleShort();
  else if (e == BTN_LONG) handleLong();

  if (!_present) return;

  static uint32_t nextDraw = 0;
  if ((int32_t)(millis() - nextDraw) < 0) return;
  nextDraw = millis() + 100;

  // Live screens rebuild every frame so readings stay current.
  Screen s = scrStack[depth];
  if (!editing && (s == SCR_SENSORS || s == SCR_NETWORK || s == SCR_CALIB)) {
    uint8_t keep = selStack[depth];
    rebuild();
    selStack[depth] = keep < nItems ? keep : (nItems ? nItems - 1 : 0);
  }

  oled.clearDisplay();
  if (s == SCR_HOME) drawHome();
  else drawMenu();
  drawToast();
  oled.display();
}
