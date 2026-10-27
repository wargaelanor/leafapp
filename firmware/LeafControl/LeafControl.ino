#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <NetBIOS.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <esp_sleep.h>
#include "configuration.h"
#include "cliParser.h"
#include "canLeafZE1.h"
#include "evCanLeaf.h"
#include "ui.h"
#include "webapp_html.h"
#include "mqttInterface.h"
#include "modemManager.h"

WebServer server(80);
DNSServer dnsServer;
bool captivePortalActive = false;
unsigned long rebootPendingAt = 0;


// State variables
bool wifiEnabled = false;
bool carIsAwake = false;

// Values read from car
float currentSOC = -1.0;
float cabinTemp = -99.0;
bool isChargingNow = false;
ChargerState currentChargerState = ChargerState::IDLE;
bool isHvacOn = false;
int8_t lockState = -1;
bool headlightsState = false;
float currentSetpoint = -1.0;
float currentFanSpeed = -1.0;
bool isHeatingEnabled = false;
bool isCoolingEnabled = false;
VentilationMode currentVentilationMode = VentilationMode::UNKNOWN;
EvBatteryData evData;

// Deep sleep state (RTC memory survives deep sleep)
struct RtcState {
  uint32_t magic;
  uint64_t idleSec;           // seconds since last car activity
  float cachedSOC;
  float cachedCabin;
  float cachedSetpoint;
  float cachedFan;
  float cachedPackV;
  float cachedPackI;
  float cachedAmbient;
  int16_t cachedGids;
  int8_t cachedSOH;
  int8_t cachedLock;
  int8_t cachedVehicleOn;
  uint8_t cachedCharger;
  uint8_t cachedFlags;        // bit0 charging, bit1 hvac, bit2 heat, bit3 cool, bit4 packValid
  uint8_t cachedVent;
};
RTC_DATA_ATTR RtcState rtcState = {0};
bool wokeFromTimer = false;

void noteCarActivity() {
  rtcState.idleSec = 0;
}

// Restore cached state after deep sleep wake
void restoreFromRtc() {
  if (rtcState.magic != 0xA71EE501) return;
  currentSOC = rtcState.cachedSOC;
  cabinTemp = rtcState.cachedCabin;
  currentSetpoint = rtcState.cachedSetpoint;
  currentFanSpeed = rtcState.cachedFan;
  lockState = rtcState.cachedLock;
  currentChargerState = (ChargerState)rtcState.cachedCharger;
  isChargingNow = rtcState.cachedFlags & 0x01;
  isHvacOn = rtcState.cachedFlags & 0x02;
  isHeatingEnabled = rtcState.cachedFlags & 0x04;
  isCoolingEnabled = rtcState.cachedFlags & 0x08;
  currentVentilationMode = (VentilationMode)rtcState.cachedVent;
  evData.soh = rtcState.cachedSOH;
  evData.gids = rtcState.cachedGids;
  evData.packVoltage = rtcState.cachedPackV;
  evData.packCurrent = rtcState.cachedPackI;
  evData.packDataValid = rtcState.cachedFlags & 0x10;
  evData.ambientTemp = rtcState.cachedAmbient;
  evData.vehicleOn = rtcState.cachedVehicleOn;

  // Push restored values into MQTT dirty flags so reconnect sync republishes them
  if (currentSOC >= 0) mqttUpdateSOC(currentSOC);
  if (cabinTemp > -30) mqttUpdateCabinTemp(cabinTemp);
  mqttUpdateCharging(isChargingNow, currentChargerState);
  mqttUpdateHVAC(isHvacOn);
  if (currentSetpoint >= 0) mqttUpdateHVACSetpoint(currentSetpoint);
  if (currentFanSpeed >= 0) mqttUpdateFanSpeed(currentFanSpeed);
  mqttUpdateHeatingMode(isHeatingEnabled, isCoolingEnabled);
  mqttUpdateVentilationMode(currentVentilationMode);
  if (lockState >= 0) mqttUpdateLock(lockState == 1);
  if (evData.soh > 0) mqttUpdateSOH(evData.soh);
  if (evData.gids > 0) mqttUpdateGids(evData.gids);
  if (evData.packDataValid) mqttUpdatePackStatus(evData.packVoltage, evData.packCurrent);
  if (evData.ambientTemp > -90) mqttUpdateAmbient(evData.ambientTemp);
  if (evData.vehicleOn >= 0) mqttUpdateVehicleOn(evData.vehicleOn == 1);
}

// Save current state to RTC memory
void saveToRtc() {
  rtcState.magic = 0xA71EE501;
  rtcState.cachedSOC = currentSOC;
  rtcState.cachedCabin = cabinTemp;
  rtcState.cachedSetpoint = currentSetpoint;
  rtcState.cachedFan = currentFanSpeed;
  rtcState.cachedPackV = evData.packVoltage;
  rtcState.cachedPackI = evData.packCurrent;
  rtcState.cachedAmbient = evData.ambientTemp;
  rtcState.cachedGids = evData.gids;
  rtcState.cachedSOH = evData.soh;
  rtcState.cachedLock = lockState;
  rtcState.cachedVehicleOn = evData.vehicleOn;
  rtcState.cachedCharger = (uint8_t)currentChargerState;
  rtcState.cachedFlags = (isChargingNow ? 0x01 : 0x00)
                       | (isHvacOn ? 0x02 : 0x00)
                       | (isHeatingEnabled ? 0x04 : 0x00)
                       | (isCoolingEnabled ? 0x08 : 0x00)
                       | (evData.packDataValid ? 0x10 : 0x00);
  rtcState.cachedVent = (uint8_t)currentVentilationMode;
}

void resetData() {
    currentSOC = -1.0;
    cabinTemp = -99.0;
    isChargingNow = false;
    currentChargerState = ChargerState::IDLE;
    isHvacOn = false;
    lockState = -1;
    currentSetpoint = -1.0;
    currentFanSpeed = -1.0;
    isHeatingEnabled = false;
    isCoolingEnabled = false;
    currentVentilationMode = VentilationMode::UNKNOWN;
}

const char* chargerStateStr(ChargerState s) {
  switch (s) {
    case ChargerState::CHARGING: return "charging";
    case ChargerState::FINISHED: return "done";
    case ChargerState::INTERRUPTED: return "stopped";
    case ChargerState::WAITING: return "wait";
    default: return "";
  }
}

void handleRoot() {
  noteCarActivity();
  server.send_P(200, "text/html", INDEX_HTML);
}

// Vehicle picture for the PWA (also usable from GitHub Pages, where the file
// is served statically). WebP is ~16.9 KB vs 23.5 KB for the PNG; kept in
// PROGMEM and streamed in chunks to avoid a large RAM buffer.
void handleLeafImg() {
  server.setContentLength(LEAF_IMG_LEN);
  server.sendHeader("Cache-Control", "public, max-age=604800");
  server.send(200, LEAF_IMG_TYPE, "");
  server.sendContent_P((const char*)LEAF_IMG, LEAF_IMG_LEN);
}

// Old name kept alive: stale PWA copies (pre leaf-v13) still ask for /leaf.png.
void handleLeafPngLegacy() {
  server.sendHeader("Location", "/leaf.webp", true);
  server.send(301, "text/plain", "");
}

void handleApiStatus() {
  noteCarActivity();
  bool sequenceActive = (activeSequence != CanSequence::NONE);
  float gLat = 0, gLon = 0, gAlt = 0, gSpd = 0;
  uint32_t gAge = 0;
  bool gpsValid = modemGpsFix(&gLat, &gLon, &gAlt, &gSpd, &gAge);
  char json[900];
  snprintf(json, sizeof(json),
    "{\"soc\":%.1f,\"cabin\":%.1f,\"charging\":%d,\"charger\":\"%s\",\"hvac\":%d,"
    "\"lock\":%d,\"setpoint\":%.1f,\"fan\":%.0f,\"heat\":%d,\"cool\":%d,\"vent\":%d,"
    "\"soh\":%d,\"gids\":%d,\"v\":%.1f,\"i\":%.1f,\"pack\":%d,\"amb\":%.1f,"
    "\"isoc\":%.1f,\"irange\":%d,\"btemp\":%.1f,\"cdelta\":%d,"
    "\"on\":%d,\"seq\":%d,\"locking\":%d,\"lights\":%d,"
    "\"modem\":\"%s\",\"gps\":{\"lat\":%.5f,\"lon\":%.5f,\"age\":%lu}}",
    currentSOC, cabinTemp, isChargingNow ? 1 : 0, chargerStateStr(currentChargerState),
    isHvacOn ? 1 : 0, (int)lockState, currentSetpoint, currentFanSpeed,
    isHeatingEnabled ? 1 : 0, isCoolingEnabled ? 1 : 0, (int)currentVentilationMode,
    evData.soh, evData.gids, evData.packVoltage, evData.packCurrent,
    evData.packDataValid ? 1 : 0, evData.ambientTemp,
    evData.instrumentSOC, evData.instrumentRange, evData.batteryTemp, evData.cellDeltaMv,
    (int)evData.vehicleOn,
    sequenceActive ? 1 : 0, getLockingEnabled() ? 1 : 0, headlightsState ? 1 : 0,
    modemGetStateStr(), gpsValid ? gLat : 0.0, gpsValid ? gLon : 0.0,
    (unsigned long)(gpsValid ? gAge / 1000UL : 0UL));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

// ---------- OTA ----------
bool otaInProgress = false;
bool arduinoOtaBegun = false;

void handleOtaPage() {
  server.send(200, "text/html",
    "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'></head><body style='font-family:Arial;text-align:center;margin-top:30px'>"
    "<h1>Firmware Update</h1>"
    "<p>Загрузите .bin, собранный через Arduino CLI/IDE<br>(File > Export Compiled Binary)</p>"
    "<form method='POST' action='/ota_update' enctype='multipart/form-data' style='margin:20px auto;max-width:320px'>"
    "<input type='file' name='firmware' accept='.bin' required style='width:100%;margin-bottom:15px'>"
    "<input type='submit' value='Update' style='padding:12px 40px;font-size:16px;background:#0077ac;color:#fff;border:none;border-radius:6px;cursor:pointer'>"
    "</form>"
    "<p id='st' style='color:#666'>Ожидание файла…</p>"
    "<script>document.forms[0].onsubmit=function(){document.getElementById('st').textContent='Прошивка, не выключайте плату…';}</script>"
    "<a href='/'>Назад</a></body></html>");
}

void handleOtaUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaInProgress = true;
    Serial.printf("### OTA upload start: %s ###\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("### OTA success: %u bytes ###\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
    otaInProgress = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaInProgress = false;
    Serial.println("### OTA aborted ###");
  }
}

void handleOtaResult() {
  if (Update.hasError()) {
    server.send(500, "text/plain", "Update failed — смотрите /ota или serial-лог");
  } else {
    server.send(200, "text/plain", "OK, перезагрузка…");
    rebootPendingAt = millis() + 1500;
  }
}

void setupArduinoOTA() {
  ArduinoOTA.setHostname(getHostName());
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    Serial.println("### ArduinoOTA start ###");
  });
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    Serial.println("\n### ArduinoOTA end ###");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int lastPct = -1;
    int pct = (int)(progress * 100 / total);
    if (pct != lastPct) {
      lastPct = pct;
      Serial.printf("ArduinoOTA %d%%\n", pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("### ArduinoOTA error %u ###\n", error);
  });
  ArduinoOTA.begin();
  arduinoOtaBegun = true;
  Serial.print("ArduinoOTA ready: ");
  Serial.println(ArduinoOTA.getHostname());
}

void handleSettings() {
  const char* result = server.hasArg("saved") ? "Saved. Rebooting..." : nullptr;
  sendSettingsPage(server, mqttIsConnected(), result);
}

void handleSettingsSave() {
  Serial.println("### Settings saved via Web UI ###");

  String serverAddr = server.arg("mqtt_server");
  serverAddr.trim();
  setMqttServer(serverAddr.c_str());

  long port = server.arg("mqtt_port").toInt();
  if (port >= 1 && port <= 65535) {
    setMqttPort((int)port);
  }

  String user = server.arg("mqtt_user");
  user.trim();
  setMqttUser(user.c_str());

  // Empty password field = keep current password
  if (server.hasArg("mqtt_password") && server.arg("mqtt_password").length() > 0) {
    setMqttPassword(server.arg("mqtt_password").c_str());
  }

  String vid = server.arg("vehicle_id");
  vid.trim();
  if (vid.length() > 0) {
    setVehicleId(vid.c_str());
  }

  // Wi-Fi STA (empty SSID = STA disabled)
  String wifiSsid = server.arg("wifi_ssid");
  wifiSsid.trim();
  setWifiSSID(wifiSsid.c_str());
  // Empty password field = keep current password
  if (server.hasArg("wifi_password") && server.arg("wifi_password").length() > 0) {
    setWifiPassword(server.arg("wifi_password").c_str());
  }

  // Wi-Fi AP (empty SSID = AP disabled)
  String apSsid = server.arg("ap_ssid");
  apSsid.trim();
  setApSSID(apSsid.c_str());
  if (server.hasArg("ap_password") && server.arg("ap_password").length() > 0) {
    ConfigStatus apPassStatus = setApPassword(server.arg("ap_password").c_str());
    if (apPassStatus != ConfigStatus::OK) {
      Serial.println("AP password rejected (must be >= 8 chars), keeping previous");
    }
  }

  setMqttTls(server.hasArg("mqtt_tls"));
  setHassDiscoveryEnabled(server.hasArg("hass_discovery"));
  setLockingEnabled(server.hasArg("locking_enabled"));

  setSleepEnabled(server.hasArg("sleep_enable"));
  int sah = server.arg("sleep_after_h").toInt();
  if (sah >= 1 && sah <= 168) setSleepAfterHours(sah);
  int wfm = server.arg("wake_fast_min").toInt();
  if (wfm >= 1 && wfm <= 1440) setWakeFastMinutes(wfm);
  int wsm = server.arg("wake_slow_min").toInt();
  if (wsm >= 1 && wsm <= 1440) setWakeSlowMinutes(wsm);

  bool m800 = server.hasArg("modem_sim800");
  bool m7670 = server.hasArg("modem_a7670");
  setModemEnable(m800 || m7670);
  setModemType(m7670 ? 1 : 0);
  int pwr = server.arg("modem_pwrkey").toInt();
  if (pwr >= 0 && pwr <= 48) setModemPwrKey(pwr);
  setModem7670Gps(server.hasArg("modem7670_gps"));
  setModemCallEnabled(server.hasArg("modem_call_enable"));
  for (uint8_t i = 0; i < 5; i++) {
    String key = "modem_call_num" + String(i + 1);
    String number = server.arg(key);
    number.trim();
    setModemCallNumber(i, number.c_str());
  }

  String m800Apn = server.arg("modem800_apn");
  m800Apn.trim();
  setModem800Apn(m800Apn.c_str());
  String m800User = server.arg("modem800_user");
  m800User.trim();
  if (m800User.length() > 0) setModem800User(m800User.c_str());
  if (server.hasArg("modem800_pass") && server.arg("modem800_pass").length() > 0) {
    setModem800Pass(server.arg("modem800_pass").c_str());
  }
  String m7670Apn = server.arg("modem7670_apn");
  m7670Apn.trim();
  setModem7670Apn(m7670Apn.c_str());
  String m7670User = server.arg("modem7670_user");
  m7670User.trim();
  if (m7670User.length() > 0) setModem7670User(m7670User.c_str());
  if (server.hasArg("modem7670_pass") && server.arg("modem7670_pass").length() > 0) {
    setModem7670Pass(server.arg("modem7670_pass").c_str());
  }

  Serial.print("mqtt_server=");
  Serial.print(getMqttServer());
  Serial.print(" mqtt_port=");
  Serial.print(getMqttPort());
  Serial.print(" vehicle_id=");
  Serial.print(getVehicleId());
  Serial.print(" wifi_ssid=");
  Serial.print(getWifiSSID());
  Serial.print(" ap_ssid=");
  Serial.println(getApSSID());

  rebootPendingAt = millis() + 2000;
  server.sendHeader("Location", "/settings?saved=1");
  server.send(303);
}

void handleCarAwake() {
  noteCarActivity();
  carIsAwake = true;
}

void handleChargerStatus(bool isCharging, ChargerState state) {
  noteCarActivity();
  isChargingNow = isCharging;
  currentChargerState = state;
  mqttUpdateCharging(isCharging, state);
}

void handleHVACStatus(bool isOn) {
  noteCarActivity();
  isHvacOn = isOn;
  
  mqttUpdateHVAC(isOn);
}

void handleCabinTemp(float temp) {
  noteCarActivity();
  cabinTemp = temp;
  mqttUpdateCabinTemp(temp);
}

void handleHVACSetpoint(float setpoint) {
  noteCarActivity();
  // The car reports 0 for setpoint during remote climate. Ignore it if we already have a valid setpoint.
  if ((isHvacOn || activeSequence == CanSequence::HVAC_ON) && setpoint == 0.0f && currentSetpoint > 0.0f) {
      return;
  }
  currentSetpoint = setpoint;
  mqttUpdateHVACSetpoint(setpoint);
}

void handleFanSpeed(float speed) {
  noteCarActivity();
  // The car reports 0 for fan speed during remote climate even though it is blowing.
  if ((isHvacOn || activeSequence == CanSequence::HVAC_ON) && speed == 0.0f && currentFanSpeed > 0.0f) {
      return;
  }
  currentFanSpeed = speed;
  mqttUpdateFanSpeed(speed);
}

void handleHeatingMode(bool heating, bool cooling) {
  noteCarActivity();
  isHeatingEnabled = heating;
  isCoolingEnabled = cooling;
  mqttUpdateHeatingMode(heating, cooling);
}

void handleVentilationMode(VentilationMode mode) {
  noteCarActivity();
  currentVentilationMode = mode;
  mqttUpdateVentilationMode(mode);
}

void handleDoorStatus(bool fl, bool fr, bool rl, bool rr, bool trunk) {
  noteCarActivity();
  mqttUpdateDoors(fl, fr, rl, rr, trunk);
}

void handleLockStatus(bool locked) {
  noteCarActivity();
  lockState = locked ? 1 : 0;
  mqttUpdateLock(locked);
}

void handleHeadlightsStatus(bool on) {
  noteCarActivity();
  headlightsState = on;
  mqttUpdateHeadlights(on);
}

void handleRawSOC(float soc) {
  noteCarActivity();
  currentSOC = soc;
  mqttUpdateSOC(soc);
}

// EV-CAN handlers (passive listener). Throttled: frames arrive at high rate.
// Pattern: keep last MQTT-published value; next frame after throttle window retries.
void handleEvSOH(int sohPercent) {
  noteCarActivity();
  static unsigned long lastPub = 0;
  static int lastPubbed = -1;
  evData.soh = sohPercent;
  if (sohPercent == lastPubbed) return;
  if (lastPub != 0 && millis() - lastPub < 30000) return;
  lastPub = millis();
  lastPubbed = sohPercent;
  mqttUpdateSOH(sohPercent);
}

void handleEvGids(int gids) {
  noteCarActivity();
  static unsigned long lastPub = 0;
  static int lastPubbed = -1;
  evData.gids = gids;
  if (gids == lastPubbed) return;
  if (lastPub != 0 && millis() - lastPub < 30000) return;
  lastPub = millis();
  lastPubbed = gids;
  mqttUpdateGids(gids);
}

void handleEvPackStatus(float voltage, float current) {
  noteCarActivity();
  static unsigned long lastPub = 0;
  static float lastPubbedV = -1.0f;
  static float lastPubbedA = 0.0f;
  evData.packVoltage = voltage;
  evData.packCurrent = current;
  evData.packDataValid = true;
  if (voltage == lastPubbedV && current == lastPubbedA) return;
  if (lastPub != 0 && millis() - lastPub < 5000) return;
  lastPub = millis();
  lastPubbedV = voltage;
  lastPubbedA = current;
  mqttUpdatePackStatus(voltage, current);
}

void handleEvAmbient(float ambientTemp) {
  noteCarActivity();
  static unsigned long lastPub = 0;
  static float lastPubbed = -99.0f;
  evData.ambientTemp = ambientTemp;
  if (ambientTemp == lastPubbed) return;
  if (lastPub != 0 && millis() - lastPub < 60000) return;
  lastPub = millis();
  lastPubbed = ambientTemp;
  mqttUpdateAmbient(ambientTemp);
}

void handleEvInstrumentSOC(float soc) {
  noteCarActivity();
  static unsigned long lastPub = 0;
  static float lastPubbed = -1.0f;
  evData.instrumentSOC = soc;
  if (soc == lastPubbed) return;
  if (lastPub != 0 && millis() - lastPub < 10000) return;
  lastPub = millis();
  lastPubbed = soc;
  mqttUpdateInstrumentSoc(soc);
}

void handleInstrumentRange(int km) {
  noteCarActivity();
  static unsigned long lastPub = 0;
  static int lastPubbed = -1;
  evData.instrumentRange = km;
  if (km == lastPubbed) return;
  if (lastPub != 0 && millis() - lastPub < 30000) return;
  lastPub = millis();
  lastPubbed = km;
  mqttUpdateInstrumentRange(km);
}

void handleBatteryTemp(float temp) {
  noteCarActivity();
  static unsigned long lastPub = 0;
  static float lastPubbed = -99.0f;
  evData.batteryTemp = temp;
  if (temp == lastPubbed) return;
  if (lastPub != 0 && millis() - lastPub < 30000) return;
  lastPub = millis();
  lastPubbed = temp;
  mqttUpdateBatteryTemp(temp);
}

void handleEvVehicleOn(bool on) {
  noteCarActivity();
  int8_t v = on ? 1 : 0;
  if (evData.vehicleOn == v) return;
  evData.vehicleOn = v;
  mqttUpdateVehicleOn(on);
}

void logSequenceStart() {
  Serial.println("### Starting Sequence ###");
  Serial.println("# type(<,>,!) time(s) identifier data...");
}


// Busy helpers: a second command during an active sequence is rejected
// explicitly instead of being silently dropped.
static bool startSeqOrBusy(CanSequence seq) {
  if (startSequence(seq, millis())) {
    logSequenceStart();
    return true;
  }
  Serial.println("### Busy: command rejected, sequence still active ###");
  return false;
}
static void sendBusyHttp() {
  server.send(409, "text/plain", "Busy: previous command still running");
}
static void sendBusyMqtt() {
  mqttPublishStatus("Busy: previous command still running");
}

// Update data (Wake up)
void handleRefresh() {
  Serial.println("### Refresh! ###");
  if (!startSeqOrBusy(CanSequence::REFRESH)) { sendBusyHttp(); return; }
  resetData();
  server.sendHeader("Location", "/");
  server.send(303);
}

bool handleMqttRefresh() {
  Serial.println("### Refresh! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::REFRESH)) { sendBusyMqtt(); return false; }
  resetData();
  return true;
}

void handleHvacOn() {
  Serial.println("### HVAC ON! ###");
  float setpoint = 0.0f;
  if (server.hasArg("setpoint")) {
    setpoint = server.arg("setpoint").toFloat();
    // Reject garbage/out-of-range setpoints from the URL
    if (setpoint != 0.0f && (setpoint < 16.0f || setpoint > 32.0f)) setpoint = 0.0f;
  }
  Serial.print("Setpoint: ");
  Serial.println(setpoint);
  if (!startSeqOrBusy(CanSequence::HVAC_ON)) { sendBusyHttp(); return; }
  setHVACTargetTemperature(setpoint);
  if (setpoint > 0.0f) {
      handleHVACSetpoint(setpoint); // Pre-fill the UI/MQTT
  }
  server.sendHeader("Location", "/"); 
  server.send(303);
}

void handleMqttHvacOn(float setpoint) {
  if (setpoint != 0.0f && (setpoint < 16.0f || setpoint > 32.0f)) setpoint = 0.0f;
  Serial.print("### HVAC ON! (MQTT) Setpoint: ");
  Serial.println(setpoint);
  if (!startSeqOrBusy(CanSequence::HVAC_ON)) { sendBusyMqtt(); return; }
  setHVACTargetTemperature(setpoint);
  if (setpoint > 0.0f) {
      handleHVACSetpoint(setpoint); // Pre-fill the UI/MQTT with the requested setpoint
  }
}

void handleHvacOff() {
  Serial.println("### HVAC OFF! ###");
  if (!startSeqOrBusy(CanSequence::HVAC_OFF)) { sendBusyHttp(); return; }
  server.sendHeader("Location", "/"); 
  server.send(303);
}

void handleMqttHvacOff() {
  Serial.println("### HVAC OFF! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::HVAC_OFF)) { sendBusyMqtt(); return; }
}

void handleMqttLock() {
  if (!getLockingEnabled()) {
    Serial.println("### Lock Doors: Feature disabled (MQTT) ###");
    mqttPublishStatus("Door locking is disabled. Enable it via CLI: set locking_enabled true");
    return;
  }
  Serial.println("### Lock Doors! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::LOCK_DOORS)) { sendBusyMqtt(); return; }
}

void handleMqttUnlock() {
  if (!getLockingEnabled()) {
    Serial.println("### Unlock Doors: Feature disabled (MQTT) ###");
    mqttPublishStatus("Door locking is disabled. Enable it via CLI: set locking_enabled true");
    return;
  }
  Serial.println("### Unlock Doors! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::UNLOCK_DOORS)) { sendBusyMqtt(); return; }
}

void handleChargeOn() {
  Serial.println("### Charge ON! ###");
  if (!startSeqOrBusy(CanSequence::CHARGE_ON)) { sendBusyHttp(); return; }
  server.sendHeader("Location", "/"); 
  server.send(303);
}

void handleUnlock() {
  if (!getLockingEnabled()) {
    Serial.println("### Unlock Doors: Feature disabled ###");
    server.send(403, "text/plain", "Door locking is disabled. Enable it via CLI: set locking_enabled true");
    return;
  }
  Serial.println("### Unlock Doors! ###");
  if (!startSeqOrBusy(CanSequence::UNLOCK_DOORS)) { sendBusyHttp(); return; }
  server.sendHeader("Location", "/"); server.send(303);
}

void handleLock() {
  if (!getLockingEnabled()) {
    Serial.println("### Lock Doors: Feature disabled ###");
    server.send(403, "text/plain", "Door locking is disabled. Enable it via CLI: set locking_enabled true");
    return;
  }
  Serial.println("### Lock Doors! ###");
  if (!startSeqOrBusy(CanSequence::LOCK_DOORS)) { sendBusyHttp(); return; }
  server.sendHeader("Location", "/"); server.send(303);
}

void handleMqttChargeOn() {
  Serial.println("### Charge ON! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::CHARGE_ON)) { sendBusyMqtt(); return; }
}

void handleChargeOff() {
  Serial.println("### Charge OFF! ###");
  if (!startSeqOrBusy(CanSequence::CHARGE_OFF)) { sendBusyHttp(); return; }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleMqttChargeOff() {
  Serial.println("### Charge OFF! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::CHARGE_OFF)) { sendBusyMqtt(); return; }
}

void handleLights() {
  Serial.println("### Lights flash! ###");
  if (!startSeqOrBusy(CanSequence::LIGHTS)) { sendBusyHttp(); return; }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleMqttLights() {
  Serial.println("### Lights flash! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::LIGHTS)) { sendBusyMqtt(); return; }
}

void handleHornLights() {
  Serial.println("### Horn + Lights! ###");
  if (!startSeqOrBusy(CanSequence::HORN_LIGHTS)) { sendBusyHttp(); return; }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleMqttHornLights() {
  Serial.println("### Horn + Lights! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::HORN_LIGHTS)) { sendBusyMqtt(); return; }
}

void handleRhlOff() {
  Serial.println("### Lights/signal OFF! ###");
  if (!startSeqOrBusy(CanSequence::RHL_OFF)) { sendBusyHttp(); return; }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleMqttRhlOff() {
  Serial.println("### Lights/signal OFF! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::RHL_OFF)) { sendBusyMqtt(); return; }
}

void manageWiFi() {
  if(!wifiEnabled) return;
  bool staEnabled = strlen(getWifiSSID()) > 0;
  static bool wifiAvailable = false;

  if (!staEnabled) {
    // AP-only mode, no STA connection to manage
    if (!wifiAvailable) {
      Serial.println("\nAP-only mode active!");
      
      if (!MDNS.begin(getHostName())) {
        Serial.println("Error starting mDNS service!");
      } else {
        Serial.println("mDNS service started.");
        MDNS.addService("http", "tcp", 80);
      }
      if (!NBNS.begin(getHostName())) {
        Serial.println("Error starting NBNS service!");
      } else {
        Serial.println("NBNS service started.");
      }

      Serial.print("You can find the UI in the browser at: http://");
      Serial.print(getHostName());
      Serial.print(".local or http://");
      Serial.print(getHostName());
      Serial.println(" or by AP IP address (usually 192.168.4.1):");
      Serial.println(WiFi.softAPIP());
      wifiAvailable = true;
    }
    return;
  }

  static unsigned long previousAttemptTime = 0;
  const unsigned long retryInterval = 10000;
  unsigned long currentTime = millis();

  if (WiFi.status() != WL_CONNECTED)
  {
    if (wifiAvailable) {
      Serial.println("WiFi disconnected, stopping mDNS/NBNS services...");
      MDNS.end();
      NBNS.end();
      wifiAvailable = false;
    }

    if (currentTime - previousAttemptTime >= retryInterval) {
      Serial.printf("WiFi connection missing, status=%d, waiting for auto-reconnect...\n", (int)WiFi.status());
      previousAttemptTime = currentTime;
    }
  }
  else if (!wifiAvailable) {
    Serial.println("\nWi-Fi connected!");
  
    if (!MDNS.begin(getHostName())) {
      Serial.println("Error starting mDNS service!");
    } else {
      Serial.println("mDNS service started.");
      MDNS.addService("http", "tcp", 80);
    }
    if (!NBNS.begin(getHostName())) {
      Serial.println("Error starting NBNS service!");
    } else {
      Serial.println("NBNS service started.");
    }

    Serial.print("You can find the UI in the browser at: http://");
    Serial.print(getHostName());
    Serial.print(".local or http://");
    Serial.print(getHostName());
    Serial.println(" or by IP address:");
    Serial.println(WiFi.localIP());
    wifiAvailable = true;
  }
}

void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.printf("[WiFi-event] event=%d\n", (int)event);
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("  STA disconnected, reason=%d\n", (int)info.wifi_sta_disconnected.reason);
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.onEvent(wifiEvent);
  initConfiguration();
  modemInit();

  wokeFromTimer = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
  if (wokeFromTimer) {
    Serial.printf("### Woke from deep sleep (idle %llu s) ###\n",
                  (unsigned long long)rtcState.idleSec);
    restoreFromRtc();
  }

  bool staEnabled = strlen(getWifiSSID()) > 0;
  bool apEnabled = strlen(getApSSID()) > 0;

  if (apEnabled) {
      size_t apPassLen = strlen(getApPassword());
      if (apPassLen > 0 && apPassLen < 8) {
          Serial.println("Error: AP password is too short (must be >= 8 chars). AP mode will be disabled.");
          apEnabled = false;
      }
  }
  
  if (staEnabled && apEnabled) {
      WiFi.mode(WIFI_AP_STA);
      WiFi.setSleep(false);
      if (strlen(getApPassword()) > 0) {
          bool apOk = WiFi.softAP(getApSSID(), getApPassword(), 1, false, 4);
          Serial.printf("softAP(%s)=%s ip=%s mode=%d\n", getApSSID(),
                        apOk ? "OK" : "FAIL", WiFi.softAPIP().toString().c_str(),
                        (int)WiFi.getMode());
      } else {
          bool apOk = WiFi.softAP(getApSSID(), "", 1, false, 4);
          Serial.printf("softAP(%s)=%s ip=%s mode=%d\n", getApSSID(),
                        apOk ? "OK" : "FAIL", WiFi.softAPIP().toString().c_str(),
                        (int)WiFi.getMode());
      }
      Serial.printf("AP status=%d SSID=%s IP=%s\n", (int)WiFi.softAPgetStationNum(),
                    getApSSID(), WiFi.softAPIP().toString().c_str());
      WiFi.setAutoReconnect(true);
      WiFi.begin(getWifiSSID(), getWifiPassword());
      WiFi.setSleep(false);
      wifiEnabled = true;
  } else if (staEnabled) {
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
      WiFi.begin(getWifiSSID(), getWifiPassword());
      WiFi.setSleep(false);
      wifiEnabled = true;
  } else if (apEnabled) {
      WiFi.mode(WIFI_AP_STA);
      int n = WiFi.scanNetworks();
      Serial.printf("scanNetworks=%d\n", n);
      for (int i = 0; i < n && i < 20; i++) {
          Serial.printf("  seen: %s ch=%d rssi=%d\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
      }
      WiFi.scanDelete();
      WiFi.mode(WIFI_AP);
      bool apOk;
      if (strlen(getApPassword()) > 0) {
          apOk = WiFi.softAP(getApSSID(), getApPassword());
      } else {
          apOk = WiFi.softAP(getApSSID());
      }
      Serial.printf("softAP(%s)=%s mode=%d status=%d\n", getApSSID(),
                    apOk ? "OK" : "FAIL", (int)WiFi.getMode(), (int)WiFi.status());
      WiFi.scanDelete();
      WiFi.setSleep(false);
      wifiEnabled = true;
  }
  else {
    // Never run with the radio fully off: with both SSIDs empty the settings
    // page would be unreachable (serial CLI only). Fall back to a config AP.
    Serial.println("WiFi disabled — starting fallback AP \"LeafControl-Setup\"");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("LeafControl-Setup");
    WiFi.setSleep(false);
    wifiEnabled = true;
  }

  setupMQTT();
  setupCAN();
  setupEvCAN();

  if (wifiEnabled) {
      server.on("/", handleRoot);
server.on("/leaf.webp", handleLeafImg);
server.on("/leaf.png", handleLeafPngLegacy);
      server.on("/api/status", handleApiStatus);
      server.on("/ota", handleOtaPage);
      server.on("/ota_update", HTTP_POST, handleOtaResult, handleOtaUpload);
      server.on("/settings", handleSettings);
      server.on("/settings_save", HTTP_POST, handleSettingsSave);
      // Commands are POST-only so a malicious page cannot trigger them via
      // <img src="http://leaf.local/lock"> style CSRF (GET) requests
      server.on("/refresh", HTTP_POST, handleRefresh);
      server.on("/hvac_on", HTTP_POST, handleHvacOn);
      server.on("/hvac_off", HTTP_POST, handleHvacOff);
      server.on("/charge_on", HTTP_POST, handleChargeOn);
      server.on("/charge_off", HTTP_POST, handleChargeOff);
      server.on("/unlock", HTTP_POST, handleUnlock);
      server.on("/lock", HTTP_POST, handleLock);
      server.on("/lights", HTTP_POST, handleLights);
      server.on("/hornlights", HTTP_POST, handleHornLights);
      server.on("/rhloff", HTTP_POST, handleRhlOff);
      server.onNotFound([]() {
        if (captivePortalActive && server.method() == HTTP_GET) {
          String host = server.hostHeader();
          if (host.length() == 0) host = WiFi.softAPIP().toString();
          server.sendHeader("Location", String("http://") + host + "/", true);
          server.send(303, "text/plain", "");
        } else {
          server.send(404, "text/plain", "Not found");
        }
      });
      server.begin();
      if ((WiFi.getMode() & WIFI_AP) != 0) {
          dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
          dnsServer.start(53, "*", WiFi.softAPIP());
          captivePortalActive = true;
          Serial.printf("Captive portal DNS active at %s\n", WiFi.softAPIP().toString().c_str());
      }
  }
}

void loop() {
  // Signed compare handles millis() wraparound; no reboot during OTA upload
  if (rebootPendingAt != 0 && !otaInProgress && (long)(millis() - rebootPendingAt) >= 0) {
    Serial.println("### Rebooting after settings save ###");
    delay(100);
    ESP.restart();
  }
  server.handleClient();
  if (captivePortalActive) dnsServer.processNextRequest();
  manageMQTT();
  modemTick();
  if (wifiEnabled && WiFi.status() == WL_CONNECTED) {
    if (!arduinoOtaBegun) setupArduinoOTA();
    if (arduinoOtaBegun) ArduinoOTA.handle();
  }
  if (!otaInProgress) {
    readAndHandleCANMessage();
    manageEvCAN();
    CanSeqResult seqRes = manageCANSequence(millis());
    if (seqRes == CanSeqResult::WAKE_SUCCESS) {
        Serial.println("### Car is awake! ###");
    } else if (seqRes == CanSeqResult::WAKE_TIMEOUT) {
        Serial.println("### Car wake timeout! ###");
    } else if (seqRes == CanSeqResult::SEQUENCE_FINISHED) {
        Serial.println("### Sequence Complete ###");
    }
  }
  manageWiFi();
  processSerialInput();

  // --- Power save: deep sleep after idle period ---
  // Never sleep while charging or running remote climate
  bool callControlKeepsAwake = getModemEnable() && getModemType() == 0 && getModemCallEnabled();
  if (getSleepEnabled() && !callControlKeepsAwake && !otaInProgress && activeSequence == CanSequence::NONE &&
      !isChargingNow && !isHvacOn) {
    static unsigned long lastIdleTickMs = 0;
    unsigned long now = millis();
    if (lastIdleTickMs == 0) lastIdleTickMs = now;
    if (now - lastIdleTickMs >= 1000) {
      uint32_t secs = (now - lastIdleTickMs) / 1000;
      rtcState.idleSec += secs;
      lastIdleTickMs += secs * 1000UL;
    }

    // After timer wake, stay awake for a grace window (web UI / MQTT publish)
    const unsigned long WAKE_GRACE_MS = 90000;
    bool graceOver = !wokeFromTimer || (millis() >= WAKE_GRACE_MS);

    if (graceOver && rtcState.idleSec >= (uint64_t)getSleepAfterHours() * 3600ULL) {
      uint64_t intervalMin = (rtcState.idleSec >= 48ULL * 3600ULL)
                             ? (uint64_t)getWakeSlowMinutes()
                             : (uint64_t)getWakeFastMinutes();
      Serial.printf("### Deep sleep: idle %llu s, wake in %llu min ###\n",
                    (unsigned long long)rtcState.idleSec,
                    (unsigned long long)intervalMin);
      modemPowerOffNow();
      saveToRtc();
      esp_sleep_enable_timer_wakeup(intervalMin * 60ULL * 1000000ULL);
      delay(100);
      esp_deep_sleep_start();
    }
  }
}
