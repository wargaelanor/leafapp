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
static const uint8_t SIM800_RI_PIN = 37;
static const uint32_t CALL_POLL_MS = 1000;

static volatile uint8_t stateVal = (uint8_t)ModemState::OFF;
static volatile bool taskBusy = false;
static bool modemPrepared = false;
static unsigned long wifiUpSinceMs = 0;
static unsigned long wifiDownSinceMs = 0;
static unsigned long failedAtMs = 0;
static unsigned long lastGpsPollMs = 0;
static unsigned long lastCallPollMs = 0;
static String pendingCaller;
static volatile int8_t callCanRequest = 0;

// Actual HVAC state decoded from EV-CAN 0x54B in LeafControl.ino.
extern bool isHvacOn;

static bool gpsHasFix = false;
static float gpsLat = 0, gpsLon = 0, gpsAlt = 0, gpsSpd = 0;
static unsigned long gpsFixMs = 0;

static void setState(ModemState s) { stateVal = (uint8_t)s; }

static const char* apnFor() { return getModem800Apn(); }
static const char* userFor() { return getModem800User(); }
static const char* passFor() { return getModem800Pass(); }

static void pulsePwrKey(bool powerOn) {
  int pin = getModemPwrKey();
  if (pin <= 0) return;
  modem800::hwPulsePwrKey(pin, powerOn);
}

static void modemHwBegin() {
  modem800::hwBegin();
}

static bool hwTestAt(uint32_t ms) { return modem800::hwTestAt(ms); }
static bool hwSimReady() { return modem800::hwSimReady(); }
static bool hwSelectNetwork(const char* apn, const char* user) {
  return modem800::hwSelectNetwork(apn, user);
}
static bool hwNetworkConnected() { return modem800::hwNetworkConnected(); }
static int16_t hwSignal() { return modem800::hwSignal(); }
static bool hwGprsConnect(const char* apn, const char* u, const char* p) {
  return modem800::hwGprsConnect(apn, u, p);
}
static bool hwGprsDisconnect() { return modem800::hwGprsDisconnect(); }

static bool callControlEnabled() {
  return getModemEnable() && getModemCallEnabled();
}

static String normalizePhone(const char* value) {
  String digits;
  if (value) {
    for (const char* p = value; *p; p++) if (*p >= '0' && *p <= '9') digits += *p;
  }
  if (digits.length() == 10) digits = "7" + digits;
  else if (digits.length() == 11 && digits[0] == '8') digits[0] = '7';
  return digits;
}

static bool callerAllowed(const String& caller) {
  String normalized = normalizePhone(caller.c_str());
  if (normalized.length() < 10) return false;
  for (uint8_t i = 0; i < 5; i++) {
    String allowed = normalizePhone(getModemCallNumber(i));
    if (allowed.length() >= 10 && allowed == normalized) return true;
  }
  return false;
}

static void attachFailed(const char* message) {
  Serial.println(message);
  setState(ModemState::FAILED);
  failedAtMs = millis();
  taskBusy = false;
  vTaskDelete(NULL);
}

static void attachTask(void*) {
  setState(ModemState::STARTING);
  Serial.println("[modem] attach start (SIM800)");

  bool needData = WiFi.status() != WL_CONNECTED;
  if (needData && strlen(apnFor()) == 0) {
    attachFailed("[modem] APN empty — cannot attach");
    return;
  }

  if (!modemPrepared) {
    modemHwBegin();
    pulsePwrKey(true);
  }

  bool atOk = false;
  unsigned long start = millis();
  while (millis() - start < ATTACH_AT_TIMEOUT_MS) {
    if (hwTestAt(2000)) { atOk = true; break; }
    vTaskDelay(1000);
  }
  if (!atOk) {
    modemPrepared = false;
    attachFailed("[modem] no AT response (boot timeout)");
    return;
  }
  Serial.println("[modem] AT sync ok");
  modemPrepared = true;

  start = millis();
  bool simOk = false;
  while (millis() - start < ATTACH_SIM_TIMEOUT_MS) {
    if (hwSimReady()) { simOk = true; break; }
    vTaskDelay(1000);
  }
  if (!simOk) {
    attachFailed("[modem] SIM not ready");
    return;
  }
  Serial.println("[modem] SIM ready");

  if (!hwSelectNetwork(apnFor(), userFor())) {
    attachFailed("[modem] operator selection failed");
    return;
  }

  start = millis();
  uint8_t probe = 0;
  bool netOk = false;
  while (millis() - start < ATTACH_NET_TIMEOUT_MS) {
    if (hwNetworkConnected()) { netOk = true; break; }
    if ((++probe % 4) == 0) Serial.printf("[modem] waiting for network... rssi=%d\n", (int)hwSignal());
    vTaskDelay(2500);
  }
  if (!netOk) {
    attachFailed("[modem] network registration timeout");
    return;
  }
  Serial.printf("[modem] network ok, rssi=%d\n", (int)hwSignal());

  if (callControlEnabled()) modem800::hwPrepareVoiceCalls();

  if (!needData) {
    setState(ModemState::STANDBY);
    Serial.println("[modem] GSM standby for incoming calls; MQTT uses WiFi");
    taskBusy = false;
    vTaskDelete(NULL);
    return;
  }

  Serial.printf("[modem] opening GPRS, APN=%s, credentials=%s...\n", apnFor(),
                strlen(userFor()) > 0 || strlen(passFor()) > 0 ? "set" : "empty");

  if (!hwGprsConnect(apnFor(), userFor(), passFor())) {
    attachFailed("[modem] GPRS attach failed");
    return;
  }
  Serial.println("[modem] GPRS up");

  setState(ModemState::READY);
  Serial.println("[modem] READY");
  taskBusy = false;
  vTaskDelete(NULL);
}

static void stopTask(void*) {
  setState(ModemState::STOPPING);
  Serial.println("[modem] stopping...");
  hwGprsDisconnect();
  pulsePwrKey(false);
  setState(ModemState::OFF);
  wifiUpSinceMs = 0;
  Serial.println("[modem] off");
  taskBusy = false;
  vTaskDelete(NULL);
}

static void standbyTask(void*) {
  setState(ModemState::STOPPING);
  Serial.println("[modem] closing GPRS; keeping GSM registered for calls");
  mqttDisconnect();
  hwGprsDisconnect();
  modem800::hwPrepareVoiceCalls();
  setState(ModemState::STANDBY);
  wifiUpSinceMs = 0;
  taskBusy = false;
  vTaskDelete(NULL);
}

static void callTask(void*) {
  setState(ModemState::CALL_ACTIVE);

  String caller = pendingCaller;
  pendingCaller = "";
  if (caller.length() == 0) {
    Serial.println("[call] incoming number unavailable; rejecting");
    modem800::hwRejectCall();
  } else if (!callerAllowed(caller)) {
    Serial.printf("[call] rejected unauthorized caller %s\n", caller.c_str());
    modem800::hwRejectCall();
  } else if (activeSequence != CanSequence::NONE) {
    Serial.println("[call] rejected authorized call: CAN sequence busy");
    modem800::hwRejectCall();
  } else {
    bool turnOn = !isHvacOn;
    Serial.printf("[call] authorized caller %s; actual climate=%s, command=%s\n",
                  caller.c_str(), isHvacOn ? "on" : "off", turnOn ? "on" : "off");
    if (modem800::hwAnswerAndPlay(turnOn ? 1 : 2)) {
      callCanRequest = turnOn ? 1 : -1;
    }
  }

  setState(WiFi.status() == WL_CONNECTED ? ModemState::STANDBY : ModemState::READY);
  taskBusy = false;
  vTaskDelete(NULL);
}

static bool startTask(TaskFunction_t fn) {
  if (taskBusy) return false;
  taskBusy = true;
  wifiUpSinceMs = 0;
  // TinyGSM can wait for SIM800 responses for tens of seconds. Priority 0 lets
  // the pinned core's idle task run, preventing the task watchdog from firing.
  if (xTaskCreatePinnedToCore(fn, "modem", 8192, nullptr, 0, nullptr, 0) != pdPASS) {
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
  Serial.println("[modem] early init (SIM800-only)");
  if (callControlEnabled()) {
    pinMode(SIM800_RI_PIN, INPUT_PULLUP);
    Serial.printf("[call] incoming calls polled by AT+CLCC; GPIO%u level=%d (RI interrupt disabled)\n",
                  SIM800_RI_PIN, digitalRead(SIM800_RI_PIN));
  }
  modemHwBegin();
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    if (hwTestAt(1500)) {
      modemPrepared = true;
      Serial.println("[modem] early init ok; attach deferred until WiFi is down");
      return;
    }
    if (attempt == 0) pulsePwrKey(true);
    delay(1000);
  }
  Serial.println("[modem] early AT failed; will retry on attach");
}

void modemTick() {
  ModemState st = (ModemState)stateVal;

  int8_t requested = callCanRequest;
  if (requested != 0) {
    callCanRequest = 0;
    bool turnOn = requested > 0;
    Serial.printf("### HVAC %s! (authorized call) ###\n", turnOn ? "ON" : "OFF");
    if (!startSequence(turnOn ? CanSequence::HVAC_ON : CanSequence::HVAC_OFF, millis())) {
      Serial.println("[call] climate command rejected: CAN sequence busy");
    }
  }

  if (!getModemEnable()) {
    if ((st == ModemState::READY || st == ModemState::STANDBY || st == ModemState::FAILED) && !taskBusy) {
      startTask(stopTask);
    }
    return;
  }

  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  unsigned long now = millis();

  if (callControlEnabled() && (st == ModemState::READY || st == ModemState::STANDBY) &&
      !taskBusy && now - lastCallPollMs >= CALL_POLL_MS) {
    lastCallPollMs = now;
    String caller;
    if (modem800::hwIncomingCaller(&caller, 700)) {
      pendingCaller = caller;
      Serial.printf("[call] incoming caller %s detected by AT+CLCC\n", caller.c_str());
      mqttDisconnect();
      startTask(callTask);
      return;
    }
  }

  if (wifiUp) {
    wifiDownSinceMs = 0;
    if (st == ModemState::READY && !taskBusy) {
      if (wifiUpSinceMs == 0) wifiUpSinceMs = now;
      else if (now - wifiUpSinceMs > WIFI_GRACE_MS) {
        startTask(callControlEnabled() ? standbyTask : stopTask);
      }
    }
    if (callControlEnabled() && (st == ModemState::OFF || st == ModemState::FAILED) && !taskBusy) {
      if (st != ModemState::FAILED || now - failedAtMs >= RETRY_COOLDOWN_MS) startTask(attachTask);
    }
  } else {
    wifiUpSinceMs = 0;
    if (wifiDownSinceMs == 0) wifiDownSinceMs = now;
    if ((st == ModemState::OFF || st == ModemState::STANDBY || st == ModemState::FAILED) && !taskBusy) {
      if (now - wifiDownSinceMs < WIFI_DROP_ATTACH_MS) return;
      if (st == ModemState::FAILED && now - failedAtMs < RETRY_COOLDOWN_MS) return;
      if (activeSequence != CanSequence::NONE) return;
      startTask(attachTask);
    }
  }

}

bool modemMqttReady() {
  return getModemEnable() && (ModemState)stateVal == ModemState::READY;
}

Client* modemTcpClient() {
  if (!modemMqttReady()) return nullptr;
  return modem800::hwTcpClient();
}

void modemPowerOffNow() {
  if (taskBusy) {
    Serial.println("[modem] power-off skipped (task busy)");
    return;
  }
  ModemState st = (ModemState)stateVal;
  if (st != ModemState::READY && st != ModemState::STANDBY && st != ModemState::FAILED) return;
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
    case ModemState::STANDBY: return "standby";
    case ModemState::CALL_ACTIVE: return "call";
    case ModemState::FAILED: return "failed";
    case ModemState::STOPPING: return "stopping";
    default: return "off";
  }
}
