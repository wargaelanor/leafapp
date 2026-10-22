#include "modemManager.h"

#include <WiFi.h>
#include <Client.h>
#include "configuration.h"
#include "modemHw.h"
#include "canLeafZE1.h"
#include "mqttInterface.h"

static const uint32_t WIFI_GRACE_MS = 120000;
static const uint32_t WIFI_DROP_ATTACH_MS = 60000;
static const uint32_t RETRY_COOLDOWN_MS = 120000;
static const uint32_t GPS_POLL_MS = 20000;
static const uint32_t ATTACH_AT_TIMEOUT_MS = 90000;
static const uint32_t ATTACH_SIM_TIMEOUT_MS = 15000;
static const uint32_t ATTACH_NET_TIMEOUT_MS = 120000;

static volatile uint8_t stateVal = (uint8_t)ModemState::OFF;
static volatile bool taskBusy = false;
static unsigned long wifiUpSinceMs = 0;
static unsigned long wifiDownSinceMs = 0;
static unsigned long failedAtMs = 0;
static unsigned long lastGpsPollMs = 0;

static bool gpsHasFix = false;
static float gpsLat = 0, gpsLon = 0, gpsAlt = 0, gpsSpd = 0;
static unsigned long gpsFixMs = 0;

static bool use7670() { return getModemType() == 1; }

static void setState(ModemState s) { stateVal = (uint8_t)s; }

static const char* apnFor() { return use7670() ? getModem7670Apn() : getModem800Apn(); }
static const char* userFor() { return use7670() ? getModem7670User() : getModem800User(); }
static const char* passFor() { return use7670() ? getModem7670Pass() : getModem800Pass(); }

static void pulsePwrKey(bool powerOn) {
  int pin = getModemPwrKey();
  if (pin <= 0) return;
  if (use7670()) modem7670::hwPulsePwrKey(pin, powerOn);
  else modem800::hwPulsePwrKey(pin, powerOn);
}

static void modemHwBegin() {
  if (use7670()) modem7670::hwBegin();
  else modem800::hwBegin();
}

static bool hwTestAt(uint32_t ms) { return use7670() ? modem7670::hwTestAt(ms) : modem800::hwTestAt(ms); }
static bool hwSimReady() { return use7670() ? modem7670::hwSimReady() : modem800::hwSimReady(); }
static bool hwNetworkConnected() { return use7670() ? modem7670::hwNetworkConnected() : modem800::hwNetworkConnected(); }
static int16_t hwSignal() { return use7670() ? modem7670::hwSignal() : modem800::hwSignal(); }
static bool hwGprsConnect(const char* apn, const char* u, const char* p) {
  return use7670() ? modem7670::hwGprsConnect(apn, u, p) : modem800::hwGprsConnect(apn, u, p);
}
static bool hwGprsDisconnect() { return use7670() ? modem7670::hwGprsDisconnect() : modem800::hwGprsDisconnect(); }

static void attachTask(void*) {
  setState(ModemState::STARTING);
  Serial.printf("[modem] attach start (%s)\n", use7670() ? "A7670" : "SIM800");

  if (strlen(apnFor()) == 0) {
    Serial.println("[modem] APN empty — cannot attach");
    setState(ModemState::FAILED);
    failedAtMs = millis();
    taskBusy = false;
    vTaskDelete(NULL);
    return;
  }

  modemHwBegin();
  pulsePwrKey(true);

  bool atOk = false;
  unsigned long start = millis();
  while (millis() - start < ATTACH_AT_TIMEOUT_MS) {
    if (hwTestAt(2000)) { atOk = true; break; }
    vTaskDelay(1000);
  }
  if (!atOk) {
    Serial.println("[modem] no AT response (boot timeout)");
    setState(ModemState::FAILED);
    failedAtMs = millis();
    taskBusy = false;
    vTaskDelete(NULL);
    return;
  }
  Serial.println("[modem] AT sync ok");

  start = millis();
  bool simOk = false;
  while (millis() - start < ATTACH_SIM_TIMEOUT_MS) {
    if (hwSimReady()) { simOk = true; break; }
    vTaskDelay(1000);
  }
  if (!simOk) {
    Serial.println("[modem] SIM not ready");
    setState(ModemState::FAILED);
    failedAtMs = millis();
    taskBusy = false;
    vTaskDelete(NULL);
    return;
  }
  Serial.println("[modem] SIM ready");

  start = millis();
  uint8_t probe = 0;
  bool netOk = false;
  while (millis() - start < ATTACH_NET_TIMEOUT_MS) {
    if (hwNetworkConnected()) { netOk = true; break; }
    if ((++probe % 4) == 0) Serial.printf("[modem] waiting for network... rssi=%d\n", (int)hwSignal());
    vTaskDelay(2500);
  }
  if (!netOk) {
    Serial.println("[modem] network registration timeout");
    setState(ModemState::FAILED);
    failedAtMs = millis();
    taskBusy = false;
    vTaskDelete(NULL);
    return;
  }
  Serial.printf("[modem] network ok, rssi=%d, opening GPRS...\n", (int)hwSignal());

  if (!hwGprsConnect(apnFor(), userFor(), passFor())) {
    Serial.println("[modem] GPRS attach failed");
    setState(ModemState::FAILED);
    failedAtMs = millis();
    taskBusy = false;
    vTaskDelete(NULL);
    return;
  }
  Serial.println("[modem] GPRS up");

  if (use7670() && getModem7670Gps()) {
    if (modem7670::hwEnableGps()) Serial.println("[modem] GPS enabled");
    else Serial.println("[modem] GPS enable failed");
  }

  setState(ModemState::READY);
  Serial.println("[modem] READY");
  taskBusy = false;
  vTaskDelete(NULL);
}

static void stopTask(void*) {
  setState(ModemState::STOPPING);
  Serial.println("[modem] stopping...");
  hwGprsDisconnect();
  if (use7670()) modem7670::hwEnableGps();
  pulsePwrKey(false);
  setState(ModemState::OFF);
  wifiUpSinceMs = 0;
  Serial.println("[modem] off");
  taskBusy = false;
  vTaskDelete(NULL);
}

static bool startTask(TaskFunction_t fn) {
  if (taskBusy) return false;
  taskBusy = true;
  wifiUpSinceMs = 0;
  if (xTaskCreatePinnedToCore(fn, "modem", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
    taskBusy = false;
    Serial.println("[modem] task create failed");
    return false;
  }
  return true;
}

void modemInit() {
  if (!getModemEnable()) {
    Serial.println("[modem] disabled");
    return;
  }
  Serial.printf("[modem] enabled (%s), will attach when WiFi is down\n",
                use7670() ? "A7670" : "SIM800");
}

void modemTick() {
  ModemState st = (ModemState)stateVal;

  if (!getModemEnable()) {
    if ((st == ModemState::READY || st == ModemState::FAILED) && !taskBusy) {
      startTask(stopTask);
    }
    return;
  }

  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  unsigned long now = millis();

  if (wifiUp) {
    wifiDownSinceMs = 0;
    if (st == ModemState::READY && !taskBusy) {
      if (wifiUpSinceMs == 0) wifiUpSinceMs = now;
      else if (now - wifiUpSinceMs > WIFI_GRACE_MS) startTask(stopTask);
    }
  } else {
    wifiUpSinceMs = 0;
    if (wifiDownSinceMs == 0) wifiDownSinceMs = now;
    if ((st == ModemState::OFF || st == ModemState::FAILED) && !taskBusy) {
      if (now - wifiDownSinceMs < WIFI_DROP_ATTACH_MS) return;
      if (st == ModemState::FAILED && now - failedAtMs < RETRY_COOLDOWN_MS) return;
      if (activeSequence != CanSequence::NONE) return;
      startTask(attachTask);
    }
  }

  if (st == ModemState::READY && use7670() && getModem7670Gps() &&
      !taskBusy && now - lastGpsPollMs >= GPS_POLL_MS) {
    lastGpsPollMs = now;
    float lat = 0, lon = 0, alt = 0, spd = 0;
    if (modem7670::hwGetGps(&lat, &lon, &alt, &spd) && lat != 0.0f && lon != 0.0f) {
      gpsHasFix = true;
      gpsLat = lat; gpsLon = lon; gpsAlt = alt; gpsSpd = spd;
      gpsFixMs = now;
      Serial.printf("[modem] GPS %.5f,%.5f alt=%.0f spd=%.1f\n", lat, lon, alt, spd);
      mqttUpdateGPS(lat, lon, alt, spd);
    }
  }
}

bool modemMqttReady() {
  return getModemEnable() && (ModemState)stateVal == ModemState::READY;
}

Client* modemTcpClient() {
  if (!modemMqttReady()) return nullptr;
  return use7670() ? modem7670::hwTcpClient() : modem800::hwTcpClient();
}

void modemPowerOffNow() {
  if (taskBusy) {
    Serial.println("[modem] power-off skipped (task busy)");
    return;
  }
  ModemState st = (ModemState)stateVal;
  if (st != ModemState::READY && st != ModemState::FAILED) return;
  Serial.println("[modem] powering off before sleep");
  hwGprsDisconnect();
  pulsePwrKey(false);
  setState(ModemState::OFF);
}

bool modemGpsFix(float* lat, float* lon, float* alt, float* speed, uint32_t* ageMs) {
  if (!gpsHasFix) return false;
  if (lat) *lat = gpsLat;
  if (lon) *lon = gpsLon;
  if (alt) *alt = gpsAlt;
  if (speed) *speed = gpsSpd;
  if (ageMs) *ageMs = (uint32_t)(millis() - gpsFixMs);
  return true;
}

ModemState modemGetState() { return (ModemState)stateVal; }

const char* modemGetStateStr() {
  switch ((ModemState)stateVal) {
    case ModemState::STARTING: return "starting";
    case ModemState::READY: return "ready";
    case ModemState::FAILED: return "failed";
    case ModemState::STOPPING: return "stopping";
    default: return "off";
  }
}
