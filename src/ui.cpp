#include "ui.h"
#include "config.h"
#include "settings.h"
#include "pumps.h"
#include "sensors.h"
#include "control.h"
#include "net.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

Ui ui;

// RST is tied to 3V3 on the module, hence -1.
//
// The glass is the 2.0" ST7789 variant of the TFT+encoder combo module:
// 240x320 native portrait, mounted landscape with the encoder below it.
// If the image comes up upside-down, swap setRotation(1) for setRotation(3)
// in begin(); if colours look like a photo negative, add
// tft.invertDisplay(false) after tft.init().
static Adafruit_ST7789 tft(PIN_TFT_CS, PIN_TFT_DC, -1);

// Everything is drawn into a full-frame canvas and pushed in one blit.
// Adafruit_ST7789 keeps no framebuffer of its own, so the clear-then-redraw
// the menu code is built around would tear visibly at a 10 Hz refresh.
// 128 x 160 x 2 = 40 KB, against 320 KB of SRAM.
static GFXcanvas16 cv(TFT_W, TFT_H);

// RGB565, same palette as the web panel in webpage.h so the two surfaces read
// as one product.
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static const uint16_t C_BG    = RGB565(0x12, 0x14, 0x0f);
static const uint16_t C_PANEL = RGB565(0x1c, 0x1f, 0x18);
static const uint16_t C_LINE  = RGB565(0x2e, 0x33, 0x27);
static const uint16_t C_BAND  = RGB565(0x3d, 0x46, 0x33);
static const uint16_t C_INK   = RGB565(0xe8, 0xe6, 0xde);
static const uint16_t C_DIM   = RGB565(0x8c, 0x91, 0x82);
static const uint16_t C_OK    = RGB565(0x8f, 0xbf, 0x4a);
static const uint16_t C_ACT   = RGB565(0xe0, 0xa8, 0x2e);
static const uint16_t C_BAD   = RGB565(0xc4, 0x55, 0x3b);

// The 128x160 canvas is centred on the 320x240 glass; begin() paints the
// border once and push() never touches it, so there is nothing to flicker.
static void push() {
  tft.drawRGBBitmap((PANEL_W - TFT_W) / 2, (PANEL_H - TFT_H) / 2,
                    cv.getBuffer(), TFT_W, TFT_H);
}

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

  // With a probe attached "Solution C" is only the fallback, so show what the
  // compensation is actually using and where it came from.
  if (sensors.tempProbeOk() && !isnan(r.tempC)) snprintf(buf, sizeof(buf), "%.1fC", r.tempC);
  else strlcpy(buf, sensors.tempProbeOk() ? "--" : "no probe", sizeof(buf));
  addItem("Probe temp", K_INFO, nullptr, 0,0,0, nullptr,0,0, buf);

  addItem("Manual C",    K_FLOAT, &cfg.tempC, 5, 45, 0.5f);
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
static void drawHeader(const char* title) {
  cv.fillRect(0, 0, TFT_W, 14, C_PANEL);
  cv.drawFastHLine(0, 14, TFT_W, C_LINE);
  cv.setTextSize(1);
  cv.setTextColor(C_DIM);
  cv.setCursor(4, 4);
  cv.print(title);
}

// Shows where a reading sits inside its deadband - the same idea as the band
// under each readout on the web page.
static void drawBand(int16_t y, float val, float sp, float db, float lo, float hi) {
  const int16_t X = 4, W = TFT_W - 8;
  cv.fillRect(X, y, W, 4, C_LINE);

  int16_t bx = X + (int16_t)((sp - db - lo) / (hi - lo) * W);
  int16_t bw = (int16_t)((2.0f * db) / (hi - lo) * W);
  if (bx < X) { bw -= (X - bx); bx = X; }
  if (bx + bw > X + W) bw = X + W - bx;
  if (bw > 0) cv.fillRect(bx, y, bw, 4, C_BAND);

  if (isnan(val)) return;
  float p = (val - lo) / (hi - lo);
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  int16_t mx = X + (int16_t)(p * W);
  cv.fillRect(mx - 1, y - 3, 3, 10, fabsf(val - sp) > db ? C_BAD : C_OK);
}

static void drawReading(int16_t y, const char* label, float v,
                        const char* blank, const char* fmt) {
  char buf[16];
  cv.setTextSize(1);
  cv.setTextColor(C_DIM);
  cv.setCursor(4, y);
  cv.print(label);

  cv.setTextSize(3);
  cv.setTextColor(isnan(v) ? C_DIM : C_INK);
  cv.setCursor(4, y + 10);
  if (isnan(v)) cv.print(blank);
  else { snprintf(buf, sizeof(buf), fmt, v); cv.print(buf); }
}

static void drawHome() {
  const Reading& r = sensors.last();
  char buf[24];

  drawHeader("MINI FEEDER");
  if (settingsPendingSave()) {
    cv.setTextColor(C_ACT);
    cv.setCursor(TFT_W - 10, 4);
    cv.print('*');
  }
  if (!r.valid) {
    cv.setTextSize(1);
    cv.setTextColor(C_DIM);
    cv.setCursor(TFT_W - 58, 4);
    cv.print(F("settling"));
  }

  drawReading(20, "pH", r.ph, "--.-", "%.2f");
  drawBand(58, r.ph, cfg.phSetpoint, cfg.phDeadband, 4.0f, 8.0f);

  drawReading(70, "EC uS/cm", r.ec, "----", "%.0f");
  drawBand(108, r.ec, cfg.ecSetpoint, cfg.ecDeadband, 0.0f, 3000.0f);

  cv.drawFastHLine(0, 120, TFT_W, C_LINE);

  cv.setTextSize(1);
  cv.setCursor(4, 126);
  if (!pumps.driverOk()) {
    cv.setTextColor(C_BAD);
    cv.print(F("PUMP DRIVER OFFLINE"));
  } else if (pumps.busy()) {
    int8_t p = pumps.running();
    cv.setTextColor(C_ACT);
    snprintf(buf, sizeof(buf), "%.13s %lus", cfg.pump[p].name,
             (unsigned long)((pumps.runRemainingMs() + 999) / 1000));
    cv.print(buf);
  } else {
    cv.setTextColor(C_ACT);
    cv.print(control.statusText());
  }

  cv.setTextColor(C_DIM);
  cv.setCursor(4, 140);
  cv.print(netIpText());
  cv.setCursor(4, 150);
  cv.print(netMqttStateText());
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

  drawHeader(title);

  // 160 px of height fits ten rows where the old 64 px panel fitted five.
  const uint8_t ROWS = 10, ROW_H = 13;
  const int16_t Y0 = 18;
  uint8_t sel = selStack[depth];
  if (sel < topRow) topRow = sel;
  if (sel >= topRow + ROWS) topRow = sel - ROWS + 1;

  char val[16];
  for (uint8_t i = 0; i < ROWS && (topRow + i) < nItems; i++) {
    uint8_t idx = topRow + i;
    int16_t y = Y0 + i * ROW_H;
    bool on = (idx == sel);

    if (on) cv.fillRect(0, y - 1, TFT_W, ROW_H, C_PANEL);

    cv.setTextSize(1);
    cv.setTextColor(on ? C_INK : C_DIM);
    cv.setCursor(4, y + 2);
    cv.print(items[idx].label);

    itemValueText(items[idx], val, sizeof(val));
    if (val[0]) {
      int16_t w = (int16_t)(strlen(val) * 6);
      int16_t x = TFT_W - 5 - w;
      if (on && editing) {
        cv.fillRect(x - 3, y - 1, w + 6, ROW_H, C_OK);
        cv.setTextColor(C_BG);
      } else {
        cv.setTextColor(on ? C_OK : C_DIM);
      }
      cv.setCursor(x, y + 2);
      cv.print(val);
    }
  }

  if (nItems > ROWS) {
    const int16_t trackH = ROWS * ROW_H;
    int16_t barH = (int16_t)((int)trackH * ROWS / nItems);
    if (barH < 8) barH = 8;
    int16_t barY = Y0 + (int16_t)((trackH - barH) * topRow / (nItems - ROWS));
    cv.drawFastVLine(TFT_W - 2, Y0, trackH, C_LINE);
    cv.fillRect(TFT_W - 3, barY, 3, barH, C_DIM);
  }
}

static void drawToast() {
  if (millis() > toastUntil) return;
  int16_t w = (int16_t)(strlen(toastMsg) * 6 + 12);
  if (w > TFT_W - 8) w = TFT_W - 8;
  int16_t x = (TFT_W - w) / 2;
  int16_t y = TFT_H / 2 - 9;
  cv.fillRect(x, y, w, 18, C_PANEL);
  cv.drawRect(x, y, w, 18, C_OK);
  cv.setTextSize(1);
  cv.setTextColor(C_INK);
  cv.setCursor(x + 6, y + 6);
  cv.print(toastMsg);
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

  // miso = -1 so the SPI peripheral never claims GPIO9 - the core skips
  // spiAttachMISO() for a negative pin, which is what leaves D9 free for the
  // temperature probe. See config.h.
  SPI.begin(PIN_TFT_SCLK, TFT_MISO_UNUSED, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(240, 320);       // ST7789 native portrait dimensions
  tft.setSPISpeed(24000000);
  tft.setRotation(1);       // landscape: 320x240, encoder below the glass
  tft.fillScreen(C_BG);

  // SPI has no acknowledge, so unlike the I2C parts there is no way to tell a
  // present display from an absent one. This reports what was configured.
  _present = true;

  cv.fillScreen(C_BG);
  cv.setTextColor(C_INK);
  cv.setTextSize(2);
  cv.setCursor(8, 58);
  cv.print(F("Mini"));
  cv.setCursor(8, 78);
  cv.print(F("Feeder"));
  cv.setTextSize(1);
  cv.setTextColor(C_DIM);
  cv.setCursor(8, 104);
  cv.print(F(FW_VERSION));
  push();

  Serial.println(F("[tft] ST7789 320x240 initialised, 128x160 UI centred"));
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

  cv.fillScreen(C_BG);
  if (s == SCR_HOME) drawHome();
  else drawMenu();
  drawToast();
  push();
}
