#include "net.h"
#include "config.h"
#include "settings.h"
#include "pumps.h"
#include "sensors.h"
#include "control.h"
#include "webpage.h"

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WebServer   server(80);
static WiFiClient  wifiClient;
static PubSubClient mqtt(wifiClient);

static bool     apMode      = false;
static uint32_t nextWifiTry = 0;
static uint32_t nextMqttTry = 0;
static uint32_t nextPublish = 0;
static bool     discoverySent = false;
static char     ipText[16]   = "no wifi";
static char     baseTopic[24];

bool netApMode() { return apMode; }
const char* netIpText() { return ipText; }

const char* netWifiStateText() {
  if (apMode) return "AP mode";
  switch (WiFi.status()) {
    case WL_CONNECTED:   return "Connected";
    case WL_NO_SSID_AVAIL: return "No SSID";
    case WL_CONNECT_FAILED: return "Bad pass";
    default: return "Connecting";
  }
}

const char* netMqttStateText() {
  if (cfg.mqttHost[0] == 0) return "Off";
  return mqtt.connected() ? "Connected" : "Down";
}

// ---------------------------------------------------------------------------
// JSON payloads
// ---------------------------------------------------------------------------
static void buildState(JsonDocument& d) {
  const Reading& r = sensors.last();
  if (!isnan(r.ph))     d["ph"]     = round(r.ph * 100) / 100.0;   else d["ph"] = nullptr;
  if (!isnan(r.ec))     d["ec"]     = round(r.ec);                 else d["ec"] = nullptr;
  if (!isnan(r.phVolt)) d["phVolt"] = round(r.phVolt * 1000) / 1000.0;
  if (!isnan(r.ecVolt)) d["ecVolt"] = round(r.ecVolt * 1000) / 1000.0;

  // The temperature compensation is actually using, and whether it came from
  // the probe or from the hand-entered fallback.
  d["tempC"]      = round(sensors.tempC() * 10) / 10.0;
  d["tempAuto"]   = sensors.tempProbeOk() && r.tempValid;

  d["valid"]      = r.valid;
  d["status"]     = control.statusText();
  d["paused"]     = control.paused();
  d["running"]    = pumps.running();
  d["remainMs"]   = pumps.runRemainingMs();
  d["floatOk"]    = pumps.floatOk();
  d["driverOk"]   = pumps.driverOk();
  d["phSetpoint"] = cfg.phSetpoint;
  d["phDeadband"] = cfg.phDeadband;
  d["ecSetpoint"] = cfg.ecSetpoint;
  d["ecDeadband"] = cfg.ecDeadband;
  d["uptime"]     = millis() / 1000;

  JsonArray a = d["pumps"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    JsonObject o = a.add<JsonObject>();
    o["name"]      = cfg.pump[i].name;
    o["mode"]      = cfg.pump[i].mode;
    o["totalMl"]   = round(pumps.totalMl(i) * 10) / 10.0;
    o["dosesHour"] = pumps.dosesLastHour(i);
    o["nextSec"]   = control.nextScheduledIn(i);
    o["queued"]    = false;
  }
}

static void buildSettings(JsonDocument& d) {
  JsonArray a = d["pumps"].to<JsonArray>();
  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    JsonObject o = a.add<JsonObject>();
    o["name"]            = cfg.pump[i].name;
    o["mlPerSec"]        = cfg.pump[i].mlPerSec;
    o["doseMl"]          = cfg.pump[i].doseMl;
    o["intervalMin"]     = cfg.pump[i].intervalMin;
    o["lockoutMin"]      = cfg.pump[i].lockoutMin;
    o["mode"]            = cfg.pump[i].mode;
    o["target"]          = cfg.pump[i].target;
    o["maxDosesPerHour"] = cfg.pump[i].maxDosesPerHour;
  }
  d["phSetpoint"] = cfg.phSetpoint;
  d["phDeadband"] = cfg.phDeadband;
  d["ecSetpoint"] = cfg.ecSetpoint;
  d["ecDeadband"] = cfg.ecDeadband;
  d["mixWaitSec"] = cfg.mixWaitSec;
  d["settleSec"]  = cfg.settleSec;
  d["tempC"]      = cfg.tempC;
  d["ecCalFactor"] = cfg.ecCalFactor;
  d["wifiSsid"]   = cfg.wifiSsid;
  d["mqttHost"]   = cfg.mqttHost;
  d["mqttPort"]   = cfg.mqttPort;
  d["mqttUser"]   = cfg.mqttUser;
  d["deviceName"] = cfg.deviceName;
  // Passwords are deliberately never sent back to the browser.
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static void reply(bool ok, const char* msg = nullptr) {
  JsonDocument d;
  d["ok"] = ok;
  if (msg) d["msg"] = msg;
  String out;
  serializeJson(d, out);
  server.send(ok ? 200 : 400, "application/json", out);
}

static void hIndex() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PAGE_INDEX);
}

static void hState() {
  JsonDocument d;
  buildState(d);
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

static void hGetSettings() {
  JsonDocument d;
  buildSettings(d);
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

template <typename T>
static void clampSet(T& dst, float v, float lo, float hi) {
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  dst = (T)v;
}

static void hPostSettings() {
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { reply(false, "bad json"); return; }

  JsonArray a = d["pumps"];
  for (uint8_t i = 0; i < NUM_PUMPS && i < a.size(); i++) {
    JsonObject o = a[i];
    if (o["name"].is<const char*>())
      strlcpy(cfg.pump[i].name, o["name"], sizeof(cfg.pump[i].name));
    if (o["mlPerSec"].is<float>())
      clampSet(cfg.pump[i].mlPerSec, o["mlPerSec"].as<float>(), PUMP_MIN_ML_PER_S, 20.0f);
    if (o["doseMl"].is<float>())
      clampSet(cfg.pump[i].doseMl, o["doseMl"].as<float>(), 0.1f, PUMP_MAX_DOSE_ML);
    if (o["intervalMin"].is<int>())
      clampSet(cfg.pump[i].intervalMin, o["intervalMin"].as<float>(), 0, 10080);
    if (o["lockoutMin"].is<int>())
      clampSet(cfg.pump[i].lockoutMin, o["lockoutMin"].as<float>(), 0, 1440);
    if (o["mode"].is<int>())
      clampSet(cfg.pump[i].mode, o["mode"].as<float>(), 0, 3);
    if (o["target"].is<int>())
      clampSet(cfg.pump[i].target, o["target"].as<float>(), 0, 2);
    if (o["maxDosesPerHour"].is<int>())
      clampSet(cfg.pump[i].maxDosesPerHour, o["maxDosesPerHour"].as<float>(), 0, DOSE_HISTORY_LEN);
  }

  if (d["phSetpoint"].is<float>()) clampSet(cfg.phSetpoint, d["phSetpoint"].as<float>(), 3.0f, 9.0f);
  if (d["phDeadband"].is<float>()) clampSet(cfg.phDeadband, d["phDeadband"].as<float>(), 0.05f, 2.0f);
  if (d["ecSetpoint"].is<float>()) clampSet(cfg.ecSetpoint, d["ecSetpoint"].as<float>(), 50.0f, 5000.0f);
  if (d["ecDeadband"].is<float>()) clampSet(cfg.ecDeadband, d["ecDeadband"].as<float>(), 10.0f, 1000.0f);
  if (d["mixWaitSec"].is<int>())   clampSet(cfg.mixWaitSec, d["mixWaitSec"].as<float>(), 5, 3600);
  if (d["settleSec"].is<int>())    clampSet(cfg.settleSec, d["settleSec"].as<float>(), SETTLE_MIN_SEC, 300);
  if (d["tempC"].is<float>())      clampSet(cfg.tempC, d["tempC"].as<float>(), 0.0f, 50.0f);

  if (d["wifiSsid"].is<const char*>()) strlcpy(cfg.wifiSsid, d["wifiSsid"], sizeof(cfg.wifiSsid));
  if (d["wifiPass"].is<const char*>()) strlcpy(cfg.wifiPass, d["wifiPass"], sizeof(cfg.wifiPass));
  if (d["mqttHost"].is<const char*>()) strlcpy(cfg.mqttHost, d["mqttHost"], sizeof(cfg.mqttHost));
  if (d["mqttUser"].is<const char*>()) strlcpy(cfg.mqttUser, d["mqttUser"], sizeof(cfg.mqttUser));
  if (d["mqttPass"].is<const char*>()) strlcpy(cfg.mqttPass, d["mqttPass"], sizeof(cfg.mqttPass));
  if (d["mqttPort"].is<int>())         clampSet(cfg.mqttPort, d["mqttPort"].as<float>(), 1, 65535);

  settingsSaveNow();
  discoverySent = false;
  reply(true, "saved");
}

static void hDose() {
  int p = server.hasArg("pump") ? server.arg("pump").toInt() : -1;
  if (p < 0 || p >= NUM_PUMPS) { reply(false, "bad pump"); return; }
  float ml = server.hasArg("ml") ? server.arg("ml").toFloat() : cfg.pump[p].doseMl;
  DoseResult r = pumps.request((uint8_t)p, ml, "web");
  reply(r == DOSE_QUEUED, doseResultText(r));
}

static void hPrime() {
  int p = server.hasArg("pump") ? server.arg("pump").toInt() : -1;
  if (p < 0 || p >= NUM_PUMPS) { reply(false, "bad pump"); return; }
  uint32_t ms = server.hasArg("ms") ? (uint32_t)server.arg("ms").toInt() : 10000;
  DoseResult r = pumps.requestTimed((uint8_t)p, ms, "web");
  reply(r == DOSE_QUEUED, doseResultText(r));
}

static void hStop()  { pumps.stopAll(); reply(true, "all pumps stopped"); }

static void hPause() {
  bool on = server.hasArg("on") ? server.arg("on").toInt() != 0 : true;
  control.setPaused(on);
  reply(true, on ? "dosing paused" : "dosing resumed");
}

static void hCalPh() {
  float v = server.hasArg("value") ? server.arg("value").toFloat() : 7.0f;
  bool ok = sensors.calibratePh(v);
  reply(ok, ok ? "pH point stored" : "not ready, wait for pumps to settle");
}

static void hCalEc() {
  float v = server.hasArg("value") ? server.arg("value").toFloat() : 1413.0f;
  bool ok = sensors.calibrateEc(v);
  reply(ok, ok ? "EC calibrated" : "not ready or probe dry");
}

static void hReboot() {
  reply(true, "rebooting");
  pumps.stopAll();
  settingsSaveNow();
  delay(300);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
static void mqttPublishDiscovery() {
  if (!mqtt.connected()) return;

  char topic[96], payload[600], uid[40];
  const char* dn = cfg.deviceName;

  // Shared device block so everything lands under one Home Assistant device.
  char dev[200];
  snprintf(dev, sizeof(dev),
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Mini Feeder\",\"mf\":\"DIY\","
    "\"mdl\":\"XIAO ESP32-C5\",\"sw\":\"%s\"},\"avty_t\":\"%s/status\"",
    dn, FW_VERSION, baseTopic);

  snprintf(uid, sizeof(uid), "%s_ph", dn);
  snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/config", uid);
  snprintf(payload, sizeof(payload),
    "{\"name\":\"pH\",\"uniq_id\":\"%s\",\"stat_t\":\"%s/state\","
    "\"val_tpl\":\"{{value_json.ph}}\",\"icon\":\"mdi:ph\",\"stat_cla\":\"measurement\",%s}",
    uid, baseTopic, dev);
  mqtt.publish(topic, payload, true);

  snprintf(uid, sizeof(uid), "%s_ec", dn);
  snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/config", uid);
  snprintf(payload, sizeof(payload),
    "{\"name\":\"EC\",\"uniq_id\":\"%s\",\"stat_t\":\"%s/state\","
    "\"val_tpl\":\"{{value_json.ec}}\",\"unit_of_meas\":\"uS/cm\","
    "\"stat_cla\":\"measurement\",%s}",
    uid, baseTopic, dev);
  mqtt.publish(topic, payload, true);

  snprintf(uid, sizeof(uid), "%s_temp", dn);
  snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/config", uid);
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Temperature\",\"uniq_id\":\"%s\",\"stat_t\":\"%s/state\","
    "\"val_tpl\":\"{{value_json.tempC}}\",\"unit_of_meas\":\"\\u00b0C\","
    "\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\",%s}",
    uid, baseTopic, dev);
  mqtt.publish(topic, payload, true);

  snprintf(uid, sizeof(uid), "%s_status", dn);
  snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/config", uid);
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Status\",\"uniq_id\":\"%s\",\"stat_t\":\"%s/state\","
    "\"val_tpl\":\"{{value_json.status}}\",%s}", uid, baseTopic, dev);
  mqtt.publish(topic, payload, true);

  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    snprintf(uid, sizeof(uid), "%s_pump%u", dn, i + 1);
    snprintf(topic, sizeof(topic), "homeassistant/button/%s/config", uid);
    snprintf(payload, sizeof(payload),
      "{\"name\":\"%s dose\",\"uniq_id\":\"%s\",\"cmd_t\":\"%s/pump/%u/dose\","
      "\"payload_press\":\"\",\"icon\":\"mdi:water-pump\",%s}",
      cfg.pump[i].name, uid, baseTopic, i, dev);
    mqtt.publish(topic, payload, true);

    snprintf(uid, sizeof(uid), "%s_pump%u_total", dn, i + 1);
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/config", uid);
    snprintf(payload, sizeof(payload),
      "{\"name\":\"%s dosed\",\"uniq_id\":\"%s\",\"stat_t\":\"%s/state\","
      "\"val_tpl\":\"{{value_json.pumps[%u].totalMl}}\",\"unit_of_meas\":\"mL\","
      "\"stat_cla\":\"total_increasing\",%s}",
      cfg.pump[i].name, uid, baseTopic, i, dev);
    mqtt.publish(topic, payload, true);
  }

  snprintf(uid, sizeof(uid), "%s_pause", dn);
  snprintf(topic, sizeof(topic), "homeassistant/switch/%s/config", uid);
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Dosing paused\",\"uniq_id\":\"%s\",\"stat_t\":\"%s/state\","
    "\"val_tpl\":\"{{'ON' if value_json.paused else 'OFF'}}\","
    "\"cmd_t\":\"%s/pause\",\"pl_on\":\"1\",\"pl_off\":\"0\",%s}",
    uid, baseTopic, baseTopic, dev);
  mqtt.publish(topic, payload, true);

  discoverySent = true;
  Serial.println(F("[mqtt] discovery published"));
}

static void mqttCallback(char* topic, byte* data, unsigned int len) {
  char payload[24] = {0};
  if (len >= sizeof(payload)) len = sizeof(payload) - 1;
  memcpy(payload, data, len);

  char want[48];
  snprintf(want, sizeof(want), "%s/pause", baseTopic);
  if (strcmp(topic, want) == 0) {
    control.setPaused(payload[0] == '1' || strcasecmp(payload, "ON") == 0);
    return;
  }

  snprintf(want, sizeof(want), "%s/stop", baseTopic);
  if (strcmp(topic, want) == 0) { pumps.stopAll(); return; }

  for (uint8_t i = 0; i < NUM_PUMPS; i++) {
    snprintf(want, sizeof(want), "%s/pump/%u/dose", baseTopic, i);
    if (strcmp(topic, want) == 0) {
      float ml = (len > 0) ? atof(payload) : 0.0f;
      if (ml <= 0) ml = cfg.pump[i].doseMl;
      DoseResult r = pumps.request(i, ml, "mqtt");
      if (r != DOSE_QUEUED)
        Serial.printf("[mqtt] dose rejected: %s\n", doseResultText(r));
      return;
    }
  }
}

static void mqttConnect() {
  if (cfg.mqttHost[0] == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if ((int32_t)(millis() - nextMqttTry) < 0) return;
  nextMqttTry = millis() + 10000;

  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  // Must stay well under the watchdog timeout. PubSubClient defaults to 15 s,
  // which means an unreachable broker blocks loop() long enough to trigger a
  // reset, then retries after reboot, forever.
  mqtt.setSocketTimeout(4);
  mqtt.setKeepAlive(30);

  char lwt[48];
  snprintf(lwt, sizeof(lwt), "%s/status", baseTopic);

  bool ok = cfg.mqttUser[0]
    ? mqtt.connect(cfg.deviceName, cfg.mqttUser, cfg.mqttPass, lwt, 0, true, "offline")
    : mqtt.connect(cfg.deviceName, lwt, 0, true, "offline");

  if (!ok) { Serial.printf("[mqtt] connect failed rc=%d\n", mqtt.state()); return; }

  mqtt.publish(lwt, "online", true);

  char sub[48];
  snprintf(sub, sizeof(sub), "%s/pump/+/dose", baseTopic); mqtt.subscribe(sub);
  snprintf(sub, sizeof(sub), "%s/pause", baseTopic);       mqtt.subscribe(sub);
  snprintf(sub, sizeof(sub), "%s/stop", baseTopic);        mqtt.subscribe(sub);

  Serial.println(F("[mqtt] connected"));
  discoverySent = false;
}

static void mqttPublishState() {
  if (!mqtt.connected()) return;
  if ((int32_t)(millis() - nextPublish) < 0) return;
  nextPublish = millis() + 5000;

  JsonDocument d;
  buildState(d);
  char out[768];
  size_t n = serializeJson(d, out, sizeof(out));
  if (n == 0 || n >= sizeof(out)) return;

  char topic[48];
  snprintf(topic, sizeof(topic), "%s/state", baseTopic);
  mqtt.publish(topic, out);
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void startAp() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_FALLBACK_SSID, AP_FALLBACK_PASS);
  strlcpy(ipText, WiFi.softAPIP().toString().c_str(), sizeof(ipText));
  Serial.printf("[wifi] AP %s at %s\n", AP_FALLBACK_SSID, ipText);
}

void netReconnect() {
  nextWifiTry = 0;
  nextMqttTry = 0;
  if (apMode) { WiFi.softAPdisconnect(true); apMode = false; }
  WiFi.disconnect();
}

static void wifiTick() {
  if (apMode) return;
  if (WiFi.status() == WL_CONNECTED) {
    strlcpy(ipText, WiFi.localIP().toString().c_str(), sizeof(ipText));
    return;
  }
  if ((int32_t)(millis() - nextWifiTry) < 0) return;

  if (cfg.wifiSsid[0] == 0) { startAp(); return; }

  static uint8_t attempts = 0;
  nextWifiTry = millis() + 15000;
  strlcpy(ipText, "connecting", sizeof(ipText));
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
  Serial.printf("[wifi] connecting to %s\n", cfg.wifiSsid);

  if (++attempts >= 4) { attempts = 0; startAp(); }
}

// ---------------------------------------------------------------------------
void netBegin() {
  snprintf(baseTopic, sizeof(baseTopic), "%s",
           cfg.deviceName[0] ? cfg.deviceName : "minifeeder");

  WiFi.setHostname(baseTopic);
  wifiTick();

  server.on("/",              HTTP_GET,  hIndex);
  server.on("/api/state",     HTTP_GET,  hState);
  server.on("/api/settings",  HTTP_GET,  hGetSettings);
  server.on("/api/settings",  HTTP_POST, hPostSettings);
  server.on("/api/dose",      HTTP_POST, hDose);
  server.on("/api/prime",     HTTP_POST, hPrime);
  server.on("/api/stop",      HTTP_POST, hStop);
  server.on("/api/pause",     HTTP_POST, hPause);
  server.on("/api/cal/ph",    HTTP_POST, hCalPh);
  server.on("/api/cal/ec",    HTTP_POST, hCalEc);
  server.on("/api/reboot",    HTTP_POST, hReboot);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  Serial.println(F("[http] server started"));
}

void netTick() {
  wifiTick();
  server.handleClient();

  if (cfg.mqttHost[0]) {
    if (!mqtt.connected()) mqttConnect();
    else {
      mqtt.loop();
      if (!discoverySent) mqttPublishDiscovery();
      mqttPublishState();
    }
  }
}
