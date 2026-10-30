#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <NetBIOS.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include "configuration.h"
#include "cliParser.h"
#include "canLeafZE1.h"
#include "evCanLeaf.h"
#include "ui.h"
#include "webapp_html.h"
#include "mqttInterface.h"
#include "ovmsV2Client.h"
#include "modemManager.h"
#include "canLogger.h"

#define LEAF_FIRMWARE_VERSION "FW-2026.10.30.68-CMD"
#define LEAF_FIRMWARE_DESCRIPTION "Real car picture + light/cable/wheel overlays embedded"

WebServer server(80);
DNSServer dnsServer;
bool captivePortalActive = false;
unsigned long rebootPendingAt = 0;
static uint8_t rebootPendingReason = 0;
RTC_DATA_ATTR static uint8_t lastSoftwareRestart = 0;
// Largest gap between loop() iterations: proves whether a stalled HTTP
// response comes from a blocked main loop or only from the network.
static uint32_t maxLoopGapMs = 0;
static uint32_t lastLoopTickMs = 0;
static uint32_t developerSession = 0;
static unsigned long developerSessionSeenMs = 0;
static const unsigned long DEVELOPER_SESSION_MS = 15UL * 60UL * 1000UL;
static uint8_t currentWifiProfile = 0;
static volatile int lastWifiDisconnectReason = 0;
// Milliseconds since the STA link went down (0 while connected). Reported as
// wifi_down_s so a "board alive but unreachable" state is visible at a glance.
static unsigned long wifiStaDownSinceMs = 0;

// (Re)create the configuration AP. Used by setup and by the Wi-Fi recovery
// path, which restarts the whole radio and therefore loses the AP config.
static void startSoftAp() {
  if (strlen(getApSSID()) == 0) return;
  bool ok;
  if (strlen(getApPassword()) > 0) {
    ok = WiFi.softAP(getApSSID(), getApPassword());
  } else {
    ok = WiFi.softAP(getApSSID());
  }
  Serial.printf("softAP(%s)=%s ip=%s\n", getApSSID(), ok ? "OK" : "FAIL",
                WiFi.softAPIP().toString().c_str());
}


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
bool doorFL = false;
bool doorFR = false;
bool doorRL = false;
bool doorRR = false;
bool doorTrunk = false;
float currentSetpoint = -1.0;
float currentFanSpeed = -1.0;
bool isHeatingEnabled = false;
bool isCoolingEnabled = false;
VentilationMode currentVentilationMode = VentilationMode::UNKNOWN;
EvBatteryData evData;
unsigned long hvacTimerEndsMs = 0;
bool hvacTimerStopPending = false;

bool validHvacMinutes(uint16_t minutes) {
  return minutes == 0 || minutes == 5 || minutes == 10 || minutes == 15 ||
         minutes == 20 || minutes == 30;
}

uint16_t hvacTimerRemainingMinutes() {
  if (hvacTimerEndsMs == 0) return 0;
  long remaining = (long)(hvacTimerEndsMs - millis());
  return remaining > 0 ? (uint16_t)((remaining + 59999L) / 60000L) : 0;
}

void cancelHvacTimer() {
  hvacTimerEndsMs = 0;
  hvacTimerStopPending = false;
  mqttUpdateHvacTimer(0);
}

void armHvacTimer(uint16_t minutes) {
  cancelHvacTimer();
  if (minutes > 0) {
    hvacTimerEndsMs = millis() + (unsigned long)minutes * 60000UL;
    mqttUpdateHvacTimer(minutes);
  }
}

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
  uint32_t cachedOdometerKm;
  char cachedVin[18];
  uint8_t cachedCharger;
  uint8_t cachedFlags;        // bit0 charging, bit1 hvac, bit2 heat, bit3 cool, bit4 packValid
  uint8_t cachedVent;
};
RTC_DATA_ATTR RtcState rtcState = {0};
bool wokeFromTimer = false;
bool timerRefreshActive = false;

void noteCarActivity() {
  // A scheduled refresh wakes the CAN buses itself. Do not treat those frames
  // as user/car activity or the periodic wake would cancel the sleep schedule.
  if (timerRefreshActive) return;
  rtcState.idleSec = 0;
}

void noteUserActivity() {
  timerRefreshActive = false;
  noteCarActivity();
}

// Restore cached state after deep sleep wake
void restoreFromRtc() {
  if (rtcState.magic != 0xA71EE502) return;
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
  evData.odometerKm = rtcState.cachedOdometerKm;
  memcpy(evData.vin, rtcState.cachedVin, sizeof(evData.vin));
  evData.vin[17] = '\0';

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
  if (evData.odometerKm > 0) mqttUpdateOdometer(evData.odometerKm);
  if (strlen(evData.vin) == 17) mqttUpdateVin(evData.vin);
}

// Save current state to RTC memory
void saveToRtc() {
  rtcState.magic = 0xA71EE502;
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
  rtcState.cachedOdometerKm = evData.odometerKm;
  strncpy(rtcState.cachedVin, evData.vin, sizeof(rtcState.cachedVin) - 1);
  rtcState.cachedVin[17] = '\0';
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
  noteUserActivity();
  // A disconnected client must not hold the single-threaded WebServer for
  // 5 seconds per chunk. Abort this response on the first short write.
  const size_t htmlLen = sizeof(INDEX_HTML) - 1;
  NetworkClient& client = server.client();
  client.setTimeout(750);
  server.setContentLength(htmlLen);
  server.send(200, "text/html; charset=utf-8", "");
  for (size_t offset = 0; offset < htmlLen; offset += 1024) {
    if (!client.connected()) break;
    size_t chunk = htmlLen - offset;
    if (chunk > 1024) chunk = 1024;
    if (client.write_P(INDEX_HTML + offset, chunk) != chunk) break;
    delay(0);
  }
  client.stop();
}

// Vehicle picture and the status overlays for the PWA. They live in PROGMEM and
// are streamed in chunks, so the UI served by the board itself is complete
// without any external files.
static void streamProgmemImage(const unsigned char* data, size_t len, const char* type) {
  NetworkClient& client = server.client();
  client.setTimeout(750);
  server.setContentLength(len);
  server.sendHeader("Cache-Control", "public, max-age=604800");
  server.send(200, type, "");
  size_t offset = 0;
  while (offset < len && client.connected()) {
    size_t chunk = len - offset;
    if (chunk > 1024) chunk = 1024;
    if (client.write_P((const char*)data + offset, chunk) != chunk) break;
    offset += chunk;
    delay(0);
  }
  client.stop();
}

void handleLeafImg() {
  streamProgmemImage(LEAF_IMG, LEAF_IMG_LEN, LEAF_IMG_TYPE);
}

// The PWA asks for car-lights.png / car-cable.png / car-wheel1..4.png; serve
// whichever are embedded, 404 for the rest (the page hides a missing layer).
void handleWebAppImage() {
  String path = server.uri();
  for (size_t i = 0; i < WEBAPP_IMAGE_COUNT; i++) {
    if (path == WEBAPP_IMAGES[i].path) {
      streamProgmemImage(WEBAPP_IMAGES[i].data, WEBAPP_IMAGES[i].len, WEBAPP_IMAGES[i].type);
      return;
    }
  }
  server.send(404, "text/plain", "not found");
}

// Start a listen-only MCP2515 clock/bitrate sweep (Port A never transmits).
void handleApiEvScan() {
  noteUserActivity();
  if (!evCanIsUp()) {
    server.send(503, "application/json", "{\"error\":\"evcan_not_ready\"}");
    return;
  }
  evCanRequestScan();
  server.send(202, "application/json", "{\"scan\":\"started\"}");
}

// Render the last raw payload of a Port B ID as hex ("-" when never seen).
static void hexFromBuf(const uint8_t* buf, uint8_t len, char* out, size_t outSize) {
  if (len == 0) { snprintf(out, outSize, "-"); return; }
  size_t used = 0;
  out[0] = '\0';
  for (uint8_t i = 0; i < len && used + 4 < outSize; i++) {
    used += snprintf(out + used, outSize - used, i ? " %02X" : "%02X", buf[i]);
  }
}

static void jsonHexPayload(uint32_t id, char* out, size_t outSize) {
  uint8_t buf[8];
  uint8_t len = dbgLastFrame(id, buf);
  hexFromBuf(buf, len, out, outSize);
}

void handleApiStatus() {
  noteUserActivity();  bool sequenceActive = (activeSequence != CanSequence::NONE);
  float gLat = 0, gLon = 0, gAlt = 0, gSpd = 0;
  uint32_t gAge = 0;
  bool gpsValid = modemGpsFix(&gLat, &gLon, &gAlt, &gSpd, &gAge);
  char json[2400];
  char h1db[26], h5bc[26], h7bb[26], h79a[26], h5c0[26], h1dba[26];
  jsonHexPayload(0x1DB, h1db, sizeof(h1db));
  jsonHexPayload(0x5BC, h5bc, sizeof(h5bc));
  jsonHexPayload(0x7BB, h7bb, sizeof(h7bb));
  jsonHexPayload(0x79A, h79a, sizeof(h79a));
  uint8_t evTemp[8], evPack[8];
  hexFromBuf(evTemp, evCanRawTemp(evTemp), h5c0, sizeof(h5c0));
  hexFromBuf(evPack, evCanRawPack(evPack), h1dba, sizeof(h1dba));
  snprintf(json, sizeof(json),
    "{\"fw\":\"%s\",\"fw_desc\":\"%s\",\"soc\":%.1f,\"cabin\":%.1f,\"charging\":%d,\"charger\":\"%s\",\"hvac\":%d,"
    "\"lock\":%d,\"setpoint\":%.1f,\"fan\":%.0f,\"heat\":%d,\"cool\":%d,\"vent\":%d,"
    "\"soh\":%d,\"gids\":%d,\"v\":%.1f,\"i\":%.1f,\"power\":%.1f,\"pack\":%d,\"vvalid\":%d,\"amb\":%.1f,"
    "\"isoc\":%.1f,\"irange\":%d,\"btemp\":%.1f,\"cdelta\":%d,"
    "\"odo\":%lu,\"vin\":\"%s\",\"hvac_timer\":%u,"
    "\"on\":%d,\"seq\":%d,\"locking\":%d,\"lights\":%d,"
    "\"evcan\":%d,\"rxonly\":%d,\"fa\":%lu,\"fb\":%lu,"
    "\"evclk\":\"%s\",\"evkbps\":\"%s\",\"evscan\":%d,\"evlock\":%d,"
    "\"evchip\":%d,\"evcfg\":%d,\"evrec\":%u,\"evtec\":%u,\"eveflg\":%u,\"evbest\":%u,\"evrst\":%lu,"
    "\"twai\":%u,\"btec\":%u,\"brec\":%u,\"fage\":%lu,\"lmax\":%lu,"
    "\"bmsq\":%lu,\"bmsr\":%lu,\"vinq\":%lu,\"vinr\":%lu,"
    "\"r1db\":\"%s\",\"r5bc\":\"%s\",\"r7bb\":\"%s\",\"r79a\":\"%s\","
    "\"r5c0\":\"%s\",\"r1dba\":\"%s\","
    "\"bmslen\":%u,\"bmsgot\":%u,\"bmsgrp\":%u,\"bmsff\":%lu,"
    "\"bmsage\":%lu,\"bmsfail\":%lu,\"bmsreq\":%u,\"bmscd\":%lu,"
    "\"modem\":\"%s\",\"uptime\":%lu,\"free_heap\":%lu,\"reset_reason\":%d,\"software_restart\":%u,"
    "\"remote_mode\":%d,\"mqtt_connected\":%d,\"ovms_v2_connected\":%d,\"ovms_v2_paranoid\":%d,"
    "\"wifi_status\":%d,\"wifi_rssi\":%d,\"wifi_profile\":%u,\"wifi_disconnect\":%d,"
    "\"wifi_down_s\":%lu,"
    "\"sleep_on\":%d,\"sleep_after_h\":%d,\"wake_fast_min\":%d,\"idle_s\":%lu,"
    "\"gps\":{\"lat\":%.5f,\"lon\":%.5f,\"age\":%lu}}",
    LEAF_FIRMWARE_VERSION, LEAF_FIRMWARE_DESCRIPTION,
    currentSOC, cabinTemp, isChargingNow ? 1 : 0, chargerStateStr(currentChargerState),
    isHvacOn ? 1 : 0, (int)lockState, currentSetpoint, currentFanSpeed,
    isHeatingEnabled ? 1 : 0, isCoolingEnabled ? 1 : 0, (int)currentVentilationMode,
    evData.soh, evData.gids, evData.packVoltage, evData.packCurrent, evData.packPower,
    evData.packDataValid ? 1 : 0, evData.packVoltageValid ? 1 : 0, evData.ambientTemp,
    evData.instrumentSOC, evData.instrumentRange, evData.batteryTemp, evData.cellDeltaMv,
    (unsigned long)evData.odometerKm, evData.vin, hvacTimerRemainingMinutes(),
    (int)evData.vehicleOn,
    sequenceActive ? 1 : 0, getLockingEnabled() ? 1 : 0, headlightsState ? 1 : 0,
    evCanIsUp() ? 1 : 0, LEAF_CAN_RX_ONLY, (unsigned long)canFramesA, (unsigned long)canFramesB,
    evCanClockName(), evCanBitrateName(), evCanScanActive() ? 1 : 0, evCanIsLocked() ? 1 : 0,
    evCanChipCheckOk() ? 1 : 0, evCanConfigWritesOk() ? 1 : 0,
    (unsigned)evCanRxErrorCount(), (unsigned)evCanTxErrorCount(), (unsigned)evCanErrorFlags(),
    (unsigned)evCanScanBestFrames(),
    (unsigned long)evCanHardResetCount(),
    (unsigned)canBusState(), (unsigned)canBusTxErrors(), (unsigned)canBusRxErrors(),
    (unsigned long)evCanLastFrameAgeSec(),
    (unsigned long)maxLoopGapMs,
    (unsigned long)bmsQueryRequests(), (unsigned long)bmsQueryResponses(),
    (unsigned long)vinQueryRequests(), (unsigned long)vinQueryResponses(),
    h1db, h5bc, h7bb, h79a, h5c0, h1dba,
    (unsigned)bmsLastExpectedLength(), (unsigned)bmsLastReceivedLength(),
    (unsigned)bmsLastGroupId(), (unsigned long)bmsFirstFrames(),
    (unsigned long)bmsLastReplyAgeSec(), (unsigned long)bmsFailureCount(),
    (unsigned)bmsRequestedGroupId(), (unsigned long)bmsCooldownLeftSec(),
    modemGetStateStr(), (unsigned long)(millis() / 1000UL),
    (unsigned long)ESP.getFreeHeap(), (int)esp_reset_reason(), (unsigned int)lastSoftwareRestart,
    (int)getRemoteTransport(), mqttIsConnected() ? 1 : 0, ovmsV2IsConnected() ? 1 : 0,
    ovmsV2ParanoidIsActive() ? 1 : 0,
    (int)WiFi.status(), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
    (unsigned int)(currentWifiProfile + 1), lastWifiDisconnectReason,
    (unsigned long)(wifiStaDownSinceMs ? (millis() - wifiStaDownSinceMs) / 1000UL : 0UL),
    getSleepEnabled() ? 1 : 0, getSleepAfterHours(), getWakeFastMinutes(),
    (unsigned long)rtcState.idleSec,
    gpsValid ? gLat : 0.0, gpsValid ? gLon : 0.0,
    (unsigned long)(gpsValid ? gAge / 1000UL : 0UL));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
  maxLoopGapMs = 0;   // report the worst stall since the previous read
}

static bool developerAuthorized() {
  if (developerSession == 0 || millis() - developerSessionSeenMs > DEVELOPER_SESSION_MS) return false;
  String cookie = server.header("Cookie");
  char expected[32];
  snprintf(expected, sizeof(expected), "leafdev=%08lx", (unsigned long)developerSession);
  if (cookie.indexOf(expected) < 0) return false;
  developerSessionSeenMs = millis();
  return true;
}

static bool requireDeveloperAuth() {
  if (developerAuthorized()) return true;
  server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

void handleDeveloperLogin() {
  if (!server.hasArg("password") || server.arg("password") != "1985") {
    delay(250);
    server.send(403, "application/json", "{\"error\":\"wrong_password\"}");
    return;
  }
  developerSession = esp_random();
  if (developerSession == 0) developerSession = 1;
  developerSessionSeenMs = millis();
  char cookie[80];
  snprintf(cookie, sizeof(cookie), "leafdev=%08lx; Path=/; HttpOnly; SameSite=Strict",
           (unsigned long)developerSession);
  server.sendHeader("Set-Cookie", cookie);
  server.send(200, "application/json", "{\"ok\":true}");
}

// Read-only VIN probe, started from the developer page. Answers "ok" and the
// VIN shows up in /api/status once (and if) the VCM replies.
void handleVinProbe() {
  if (!requireDeveloperAuth()) return;
  noteUserActivity();
  requestVinObdProbe();
  server.send(200, "application/json",
              "{\"ok\":true,\"probe\":\"obd09_02\",\"request\":\"0x7DF\",\"reply\":\"0x7E8\"}");
}

void handleCanLogStatus() {
  if (!requireDeveloperAuth()) return;
  char json[180];
  snprintf(json, sizeof(json),
           "{\"recording\":%s,\"full\":%s,\"available\":%s,\"frames\":%lu,\"capacity\":%lu}",
           canLoggerIsRecording() ? "true" : "false", canLoggerIsFull() ? "true" : "false",
           canLoggerHasLog() ? "true" : "false",
           (unsigned long)canLoggerCount(), (unsigned long)canLoggerCapacity());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleCanLogStart() {
  if (!requireDeveloperAuth()) return;
  noteUserActivity();
  if (!canLoggerStart()) {
    server.send(507, "application/json", "{\"error\":\"no_memory\"}");
    return;
  }
  Serial.println("### Web CAN logging started ###");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCanLogStop() {
  if (!requireDeveloperAuth()) return;
  noteUserActivity();
  canLoggerStop();
  Serial.println("### Web CAN logging stopped ###");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCanLogClear() {
  if (!requireDeveloperAuth()) return;
  noteUserActivity();
  canLoggerClear();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCanLogDownload() {
  if (!requireDeveloperAuth()) return;
  canLoggerSendDownload();
}

// ---------- OTA ----------
bool otaInProgress = false;
// Last time an upload chunk arrived. The supervisor uses it to break out of a
// dead transfer: when a client vanishes mid-upload the web server can keep the
// main loop blocked for 10-12 minutes waiting on the half-open socket, and the
// board looks offline for that whole time.
static volatile uint32_t otaLastProgressMs = 0;
bool arduinoOtaBegun = false;
static volatile uint32_t loopHeartbeatMs = 0;
static volatile bool setupComplete = false;

static void loopSupervisorTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    if (!setupComplete) continue;
    if (otaInProgress) {
      // A client that disappears mid-upload (weak link) leaves the web server
      // waiting on a half-open socket, which stalls the loop for minutes and
      // makes the board unreachable. Reboot instead: the running image is
      // untouched because Update.end() was never reached.
      if (otaLastProgressMs != 0 && millis() - otaLastProgressMs > 120000UL) {
        Serial.println("### Supervisor: OTA stalled for 2 min, rebooting ###");
        lastSoftwareRestart = 3;
        delay(100);
        ESP.restart();
      }
      continue;
    }
    uint32_t heartbeat = loopHeartbeatMs;
    if (heartbeat != 0 && millis() - heartbeat > 60000UL) {
      Serial.println("### Supervisor: main loop stalled, rebooting ###");
      lastSoftwareRestart = 1;
      delay(100);
      ESP.restart();
    }
  }
}

void handleOtaPage() {
  server.send(200, "text/html",
    "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'></head><body style='font-family:Arial;text-align:center;margin-top:30px'>"
    "<h1>Firmware Update</h1>"
    "<p>Установлено: <strong>" LEAF_FIRMWARE_VERSION "</strong><br>SIM800: CAN-телеметрия, дистанционные команды и климат по разрешённому звонку</p>"
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
    otaLastProgressMs = millis();
    Serial.printf("### OTA upload start: %s ###\n", upload.filename.c_str());
    // A TLS connect that started just before the upload blocks the main loop
    // for its whole timeout (~30 s at -90 dBm). During that stall the upload
    // reader gets no data, the server gives up, and the transfer dies at a
    // random offset. Abort the remote links outright instead of waiting.
    mqttDisconnect();
    ovmsV2Disconnect();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    otaLastProgressMs = millis();
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
    otaLastProgressMs = 0;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaInProgress = false;
    otaLastProgressMs = 0;
    Serial.println("### OTA aborted ###");
  }
}

void handleOtaResult() {
  if (Update.hasError()) {
    server.send(500, "text/plain", "Update failed — смотрите /ota или serial-лог");
  } else {
    server.send(200, "text/plain", "OK, перезагрузка…");
    rebootPendingAt = millis() + 1500;
    rebootPendingReason = 2;
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
  sendSettingsPage(server, mqttIsConnected(), ovmsV2IsConnected(), result);
}

void handleSettingsSave() {
  Serial.println("### Settings saved via Web UI ###");

  int remoteMode = server.arg("remote_mode").toInt();
  if (remoteMode >= 0 && remoteMode <= 2) {
    setRemoteTransport(static_cast<RemoteTransport>(remoteMode));
  }

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

  // MQTT topics have their own vehicle id so the two transports can differ.
  // Empty field = keep the current value (the form always sends it, but an
  // older page might not).
  if (server.hasArg("mqtt_vehicle_id")) {
    String mqttVid = server.arg("mqtt_vehicle_id");
    mqttVid.trim();
    if (mqttVid.length() > 0) {
      setMqttVehicleId(mqttVid.c_str());
    }
  }

  // Wi-Fi STA (empty SSID = STA disabled)
  String wifiSsid = server.arg("wifi_ssid");
  wifiSsid.trim();
  setWifiSSID(wifiSsid.c_str());
  // Empty password field = keep current password
  if (server.hasArg("wifi_password") && server.arg("wifi_password").length() > 0) {
    setWifiPassword(server.arg("wifi_password").c_str());
  }

  String ovmsServer = server.arg("ovms_server");
  ovmsServer.trim();
  setOvmsServer(ovmsServer.c_str());
  int ovmsPort = server.arg("ovms_port").toInt();
  if (ovmsPort >= 1 && ovmsPort <= 65535) setOvmsPort(ovmsPort);
  if (server.hasArg("ovms_password") && server.arg("ovms_password").length() > 0) {
    setOvmsPassword(server.arg("ovms_password").c_str());
  }
  if (server.hasArg("ovms_module_password") && server.arg("ovms_module_password").length() > 0) {
    setOvmsModulePassword(server.arg("ovms_module_password").c_str());
  }
  setOvmsTls(server.hasArg("ovms_tls"));
  String wifiSsid2 = server.arg("wifi_ssid2");
  wifiSsid2.trim();
  setWifiSSID2(wifiSsid2.c_str());
  if (server.hasArg("wifi_password2") && server.arg("wifi_password2").length() > 0) {
    setWifiPassword2(server.arg("wifi_password2").c_str());
  }
  String wifiSsid3 = server.arg("wifi_ssid3");
  wifiSsid3.trim();
  setWifiSSID3(wifiSsid3.c_str());
  if (server.hasArg("wifi_password3") && server.arg("wifi_password3").length() > 0) {
    setWifiPassword3(server.arg("wifi_password3").c_str());
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
  setModemEnable(m800);
  setModemType(0);
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
  Serial.print(" mqtt_vehicle_id=");
  Serial.print(getMqttVehicleId());
  Serial.print(" wifi_ssid=");
  Serial.print(getWifiSSID());
  Serial.print(" wifi_ssid2=");
  Serial.print(getWifiSSID2());
  Serial.print(" wifi_ssid3=");
  Serial.print(getWifiSSID3());
  Serial.print(" ap_ssid=");
  Serial.println(getApSSID());

  rebootPendingAt = millis() + 2000;
  rebootPendingReason = 3;
  server.sendHeader("Location", "/settings?saved=1");
  server.send(303);
}

// Narrow endpoint for provisioning extra STA profiles without submitting the
// full settings form (where absent checkboxes intentionally mean "disabled").
void handleWifiProfilesSave() {
  for (uint8_t profile = 2; profile <= 3; profile++) {
    String suffix = String(profile);
    String ssidKey = "wifi_ssid" + suffix;
    String passwordKey = "wifi_password" + suffix;
    if (server.hasArg(ssidKey)) {
      String ssid = server.arg(ssidKey);
      ssid.trim();
      if (profile == 2) setWifiSSID2(ssid.c_str());
      else setWifiSSID3(ssid.c_str());
    }
    if (server.hasArg(passwordKey) && server.arg(passwordKey).length() > 0) {
      if (profile == 2) setWifiPassword2(server.arg(passwordKey).c_str());
      else setWifiPassword3(server.arg(passwordKey).c_str());
    }
  }
  rebootPendingAt = millis() + 2000;
  rebootPendingReason = 4;
  server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
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
  doorFL = fl;
  doorFR = fr;
  doorRL = rl;
  doorRR = rr;
  doorTrunk = trunk;
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
  evData.packPower = voltage * current / 1000.0f;
  evData.packDataValid = true;
  evData.packVoltageValid = true;
  if (voltage == lastPubbedV && current == lastPubbedA) return;
  if (lastPub != 0 && millis() - lastPub < 5000) return;
  lastPub = millis();
  lastPubbedV = voltage;
  lastPubbedA = current;
  mqttUpdatePackStatus(voltage, current);
}

void handleOdometer(uint32_t km) {
  if (evData.odometerKm == km) return;
  evData.odometerKm = km;
  mqttUpdateOdometer(km);
}

void handleVin(const char* vin) {
  if (!vin || strlen(vin) != 17) return;
  strncpy(evData.vin, vin, sizeof(evData.vin) - 1);
  evData.vin[17] = '\0';
  Serial.printf("VIN: %s\n", evData.vin);
  mqttUpdateVin(evData.vin);
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

void handleEvBmsSOC(float soc) {
  noteCarActivity();
  if (soc <= 0.0f || soc > 100.0f) return;
  static unsigned long lastPub = 0;
  static float lastPubbed = -1.0f;
  currentSOC = soc;
  if (soc == lastPubbed) return;
  if (lastPub != 0 && millis() - lastPub < 10000) return;
  lastPub = millis();
  lastPubbed = soc;
  mqttUpdateSOC(currentSOC);
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
  // READY is genuine vehicle activity, not merely the scheduled wake burst.
  if (on) timerRefreshActive = false;
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
    noteUserActivity();
    logSequenceStart();
    return true;
  }
  Serial.println("### Busy: command rejected, sequence still active ###");
  return false;
}
static void sendBusyHttp() {
  server.send(409, "text/plain", "Busy: previous command still running");
}

void handlePackVoltage(float voltage) {
  if (voltage < 200.0f || voltage > 500.0f) return;
  evData.packVoltage = voltage;
  evData.packVoltageValid = true;
}

void handleCellDelta(int millivolts) {
  if (millivolts < 0 || millivolts > 1000) return;
  evData.cellDeltaMv = millivolts;
  mqttUpdateCellDelta(millivolts);
}
static void sendCommandNotEnabledHttp() {
  server.send(501, "application/json", "{\"error\":\"command_not_enabled\"}");
}
static void sendBusyMqtt() {
  mqttPublishStatus("Busy: previous command still running");
}

// Update data (Wake up)
void handleRefresh() {
  Serial.println("### Refresh! ###");
  if (!startSeqOrBusy(CanSequence::REFRESH)) { sendBusyHttp(); return; }
  // The refresh wakes CAR-CAN, which usually wakes the rest of the car too.
  // If EV-CAN has never produced a frame, use this traffic window to sweep
  // MCP2515 clock/bitrate candidates while staying listen-only.
  if (canFramesA == 0) evCanRequestScan();
  server.sendHeader("Location", "/");
  server.send(303);
}

bool handleMqttRefresh() {
  Serial.println("### Refresh! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::REFRESH)) { sendBusyMqtt(); return false; }
  return true;
}

void handleHvacOn() {
  Serial.println("### HVAC ON! ###");
  float setpoint = 0.0f;
  if (server.hasArg("setpoint")) {
    setpoint = server.arg("setpoint").toFloat();
    if (setpoint < 16.0f || setpoint > 32.0f) {
      server.send(400, "text/plain", "Invalid HVAC setpoint");
      return;
    }
  }
  uint16_t minutes = server.hasArg("minutes") ? (uint16_t)server.arg("minutes").toInt() : 0;
  if (!validHvacMinutes(minutes)) { server.send(400, "text/plain", "Invalid HVAC duration"); return; }
  Serial.print("Setpoint: ");
  Serial.println(setpoint);
  if (!startSeqOrBusy(CanSequence::HVAC_ON)) { sendBusyHttp(); return; }
  armHvacTimer(minutes);
  setHVACTargetTemperature(setpoint);
  if (setpoint > 0.0f) {
      handleHVACSetpoint(setpoint); // Pre-fill the UI/MQTT
  }
  server.sendHeader("Location", "/"); 
  server.send(303);
}

void handleMqttHvacOn(float setpoint, uint16_t minutes) {
  if (setpoint != 0.0f && (setpoint < 16.0f || setpoint > 32.0f)) setpoint = 0.0f;
  if (!validHvacMinutes(minutes)) minutes = 0;
  Serial.print("### HVAC ON! (MQTT) Setpoint: ");
  Serial.println(setpoint);
  if (!startSeqOrBusy(CanSequence::HVAC_ON)) { sendBusyMqtt(); return; }
  armHvacTimer(minutes);
  setHVACTargetTemperature(setpoint);
  if (setpoint > 0.0f) {
      handleHVACSetpoint(setpoint); // Pre-fill the UI/MQTT with the requested setpoint
  }
}

void handleHvacOff() {
  Serial.println("### HVAC OFF! ###");
  if (!startSeqOrBusy(CanSequence::HVAC_OFF)) { sendBusyHttp(); return; }
  cancelHvacTimer();
  server.sendHeader("Location", "/"); 
  server.send(303);
}

void handleMqttHvacOff() {
  Serial.println("### HVAC OFF! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::HVAC_OFF)) { sendBusyMqtt(); return; }
  cancelHvacTimer();
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
  // No confirmed ZE1 stop-charge payload is available. Keep this route
  // explicit instead of reporting a misleading busy response.
  sendCommandNotEnabledHttp();
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
  sendCommandNotEnabledHttp();
}

void handleMqttHornLights() {
  Serial.println("### Horn + Lights! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::HORN_LIGHTS)) { sendBusyMqtt(); return; }
}

void handleRhlOff() {
  Serial.println("### Lights/signal OFF! ###");
  sendCommandNotEnabledHttp();
}

void handleMqttRhlOff() {
  Serial.println("### Lights/signal OFF! (MQTT) ###");
  if (!startSeqOrBusy(CanSequence::RHL_OFF)) { sendBusyMqtt(); return; }
}

static const char* wifiProfileSsid(uint8_t index) {
  if (index == 0) return getWifiSSID();
  if (index == 1) return getWifiSSID2();
  return getWifiSSID3();
}

uint8_t handleOvmsV2Command(uint16_t command, const char* args) {
  switch (command) {
    case 11: // Start charge
      return startSeqOrBusy(CanSequence::CHARGE_ON) ? 0 : 1;
    case 18: // Wakeup car
    case 19: // Wakeup temperature subsystem (same Leaf wakeup upstream)
      if (!startSeqOrBusy(CanSequence::REFRESH)) return 1;
      return 0;
    case 20: // Lock
      if (!getLockingEnabled()) return 1;
      return startSeqOrBusy(CanSequence::LOCK_DOORS) ? 0 : 1;
    case 22: // Unlock
      if (!getLockingEnabled()) return 1;
      return startSeqOrBusy(CanSequence::UNLOCK_DOORS) ? 0 : 1;
    case 24: { // Leaf upstream maps Homelink buttons 0/1 to climate on/off
      int button = atoi(args);
      if (button == 0) return startSeqOrBusy(CanSequence::HVAC_ON) ? 0 : 1;
      if (button == 1) return startSeqOrBusy(CanSequence::HVAC_OFF) ? 0 : 1;
      return 2;
    }
    case 26: { // Remote climate control: 1=on, 0=off
      bool enable = atoi(args) != 0;
      if (!startSeqOrBusy(enable ? CanSequence::HVAC_ON : CanSequence::HVAC_OFF)) return 1;
      if (!enable) cancelHvacTimer();
      return 0;
    }
    case 12: // Stop charge needs the upstream MITM installation, unavailable on OBD
    default:
      return 2;
  }
}

static const char* wifiProfilePassword(uint8_t index) {
  if (index == 0) return getWifiPassword();
  if (index == 1) return getWifiPassword2();
  return getWifiPassword3();
}

static bool wifiStaEnabled() {
  for (uint8_t i = 0; i < 3; i++) {
    if (strlen(wifiProfileSsid(i)) > 0) return true;
  }
  return false;
}

static void connectWifiProfile(bool rotateProfile = false) {
  int bestProfile = -1;
  int bestRssi = -1000;
  if (rotateProfile) {
    for (uint8_t step = 1; step <= 3; step++) {
      uint8_t candidate = (currentWifiProfile + step) % 3;
      if (wifiProfileSsid(candidate)[0]) { bestProfile = candidate; break; }
    }
  } else {
    int count = WiFi.scanNetworks(false, true);
    for (uint8_t profile = 0; profile < 3; profile++) {
      const char* wanted = wifiProfileSsid(profile);
      if (!wanted[0]) continue;
      for (int network = 0; network < count; network++) {
        if (WiFi.SSID(network) == wanted && WiFi.RSSI(network) > bestRssi) {
          bestProfile = profile;
          bestRssi = WiFi.RSSI(network);
        }
      }
    }
    WiFi.scanDelete();
  }
  if (bestProfile < 0) {
    // No saved SSID was visible; rotate profiles in case it is hidden or the
    // scan missed it. The connection timeout will move to the next profile.
    for (uint8_t step = 1; step <= 3; step++) {
      uint8_t candidate = (currentWifiProfile + step) % 3;
      if (wifiProfileSsid(candidate)[0]) { bestProfile = candidate; break; }
    }
  }
  if (bestProfile < 0) return;
  currentWifiProfile = (uint8_t)bestProfile;
  Serial.printf("Wi-Fi: connecting profile %u SSID=%s RSSI=%d\n",
                currentWifiProfile + 1, wifiProfileSsid(currentWifiProfile), bestRssi);
  WiFi.disconnect(false, false);
  WiFi.begin(wifiProfileSsid(currentWifiProfile), wifiProfilePassword(currentWifiProfile));
}

void manageWiFi() {
  if(!wifiEnabled) return;
  bool staEnabled = wifiStaEnabled();
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
  static unsigned long staDownSinceMs = 0;
  static uint8_t retryStep = 0;
  static const unsigned long retryBackoffMs[] = {8000, 15000, 30000, 60000};
  unsigned long currentTime = millis();

  if (WiFi.status() != WL_CONNECTED)
  {
    if (staDownSinceMs == 0) staDownSinceMs = currentTime;
    wifiStaDownSinceMs = staDownSinceMs;
    if (wifiAvailable) {
      Serial.println("WiFi disconnected, stopping mDNS/NBNS services...");
      MDNS.end();
      NBNS.end();
      wifiAvailable = false;
    }

    // Retrying every 5 s called WiFi.disconnect()+WiFi.begin() again while the
    // previous association was still in progress. At -90 dBm an association
    // needs longer than that, so the STA never finished and the board looked
    // alive (AP beaconing, visible from a phone) while being unreachable on
    // the network. Back off instead, and restart the radio if it stays dead.
    unsigned long interval = retryBackoffMs[retryStep > 3 ? 3 : retryStep];
    if (currentTime - previousAttemptTime >= interval) {
      if (currentTime - staDownSinceMs >= 300000UL) {
        Serial.println("WiFi: STA down for 5 min, restarting the radio");
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        delay(300);
        WiFi.mode(strlen(getApSSID()) > 0 ? WIFI_AP_STA : WIFI_STA);
        WiFi.setSleep(false);
        startSoftAp();
        staDownSinceMs = currentTime;
        wifiStaDownSinceMs = staDownSinceMs;
        retryStep = 0;
      }
      Serial.printf("WiFi connection missing, status=%d; trying saved profiles...\n", (int)WiFi.status());
      connectWifiProfile(true);
      previousAttemptTime = currentTime;
      if (retryStep < 3) retryStep++;
    }
  }
  else if (!wifiAvailable) {
    staDownSinceMs = 0;
    wifiStaDownSinceMs = 0;
    retryStep = 0;
    previousAttemptTime = currentTime;
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
    lastWifiDisconnectReason = (int)info.wifi_sta_disconnected.reason;
    Serial.printf("  STA disconnected, reason=%d\n", (int)info.wifi_sta_disconnected.reason);
  }
}

void setup() {
  Serial.begin(115200);
  // Arduino loopTask runs on core 1. Keep recovery on core 0 so a blocking
  // HTTP/network call cannot also starve the task responsible for rebooting.
  BaseType_t supervisorResult = xTaskCreatePinnedToCore(
      loopSupervisorTask, "loop-watch", 3072, nullptr, 1, nullptr, 0);
  if (supervisorResult != pdPASS) {
    Serial.println("### ERROR: failed to start cross-core loop supervisor ###");
  }
  Serial.printf("### Leaf Control %s: %s ###\n", LEAF_FIRMWARE_VERSION, LEAF_FIRMWARE_DESCRIPTION);
  WiFi.onEvent(wifiEvent);
  initConfiguration();
  modemInit();

  wokeFromTimer = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
  if (wokeFromTimer) {
    Serial.printf("### Woke from deep sleep (idle %llu s) ###\n",
                  (unsigned long long)rtcState.idleSec);
    restoreFromRtc();
  }

  bool staEnabled = wifiStaEnabled();
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
      WiFi.setAutoReconnect(false);
      connectWifiProfile();
      WiFi.setSleep(false);
      wifiEnabled = true;
  } else if (staEnabled) {
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(false);
      connectWifiProfile();
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
  setupOvmsV2();
  setupCAN();
  setupEvCAN();

  if (wokeFromTimer) {
    timerRefreshActive = true;
    if (startSequence(CanSequence::REFRESH, millis())) {
      Serial.println("### Timer wake: automatic vehicle refresh started ###");
      logSequenceStart();
    } else {
      timerRefreshActive = false;
      Serial.println("### Timer wake: automatic vehicle refresh failed to start ###");
    }
  }

  if (wifiEnabled) {
      const char* trackedHeaders[] = {"Cookie"};
      server.collectHeaders(trackedHeaders, 1);
      server.on("/", handleRoot);
server.on("/leaf.webp", handleLeafImg);
server.on("/leaf.png", handleLeafImg);
// status-picture overlays (only the ones present in the build are served)
for (size_t i = 0; i < WEBAPP_IMAGE_COUNT; i++) {
  server.on(WEBAPP_IMAGES[i].path, HTTP_GET, handleWebAppImage);
}
      server.on("/api/status", handleApiStatus);
      server.on("/api/evscan", HTTP_POST, handleApiEvScan);
      server.on("/api/developer/login", HTTP_POST, handleDeveloperLogin);
      server.on("/api/developer/canlog", HTTP_GET, handleCanLogStatus);
      server.on("/api/developer/vin_probe", HTTP_POST, handleVinProbe);
      server.on("/api/developer/canlog/start", HTTP_POST, handleCanLogStart);
      server.on("/api/developer/canlog/stop", HTTP_POST, handleCanLogStop);
      server.on("/api/developer/canlog/clear", HTTP_POST, handleCanLogClear);
      server.on("/api/developer/canlog/download", HTTP_GET, handleCanLogDownload);
      server.on("/ota", handleOtaPage);
      server.on("/ota_update", HTTP_POST, handleOtaResult, handleOtaUpload);
      server.on("/settings", handleSettings);
      server.on("/settings_save", HTTP_POST, handleSettingsSave);
      server.on("/api/wifi_profiles", HTTP_POST, handleWifiProfilesSave);
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
        const String& path = server.uri();
        if (server.method() == HTTP_GET &&
            (path == "/refresh" || path == "/hvac_on" || path == "/hvac_off" ||
             path == "/charge_on" || path == "/charge_off" || path == "/unlock" ||
             path == "/lock" || path == "/lights" || path == "/hornlights" ||
             path == "/rhloff")) {
          server.sendHeader("Allow", "POST");
          server.send(405, "application/json", "{\"error\":\"method_not_allowed\"}");
          return;
        }
        if (captivePortalActive && server.method() == HTTP_GET) {
          // Redirect only captive-portal probes (or a foreign Host), and always
          // to the AP's literal IP. Redirecting every unknown GET to the same
          // host loops forever and turns asset 404s into HTML, which is why the
          // page failed to load when connected straight to the board's AP.
          const String& uri = server.uri();
          bool probe = uri == "/generate_204" || uri == "/gen_204" ||
                       uri == "/hotspot-detect.html" || uri == "/library/test/success.html" ||
                       uri == "/ncsi.txt" || uri == "/connecttest.txt" ||
                       uri == "/canonical.html" || uri == "/success.txt";
          String host = server.hostHeader();
          bool foreignHost = host.length() > 0 &&
                             host != WiFi.softAPIP().toString() &&
                             host != "leaf.local";
          if (probe || foreignHost) {
            server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
            server.send(302, "text/plain", "");
            return;
          }
        }
        server.send(404, "text/plain", "Not found");
      });
      server.begin();
      if ((WiFi.getMode() & WIFI_AP) != 0) {
          dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
          dnsServer.start(53, "*", WiFi.softAPIP());
          captivePortalActive = true;
          Serial.printf("Captive portal DNS active at %s\n", WiFi.softAPIP().toString().c_str());
      }
  }
  loopHeartbeatMs = millis();
  setupComplete = true;
}

void loop() {
  loopHeartbeatMs = millis();
  {
    uint32_t nowTick = loopHeartbeatMs;
    uint32_t gap = nowTick - lastLoopTickMs;
    if (lastLoopTickMs != 0 && gap > maxLoopGapMs) maxLoopGapMs = gap;
    lastLoopTickMs = nowTick;
  }
  static uint16_t lastPublishedHvacMinutes = 0;
  // Signed compare handles millis() wraparound; no reboot during OTA upload
  if (rebootPendingAt != 0 && !otaInProgress && (long)(millis() - rebootPendingAt) >= 0) {
    Serial.println("### Rebooting after settings save ###");
    lastSoftwareRestart = rebootPendingReason;
    delay(100);
    ESP.restart();
  }
  server.handleClient();
  if (captivePortalActive) dnsServer.processNextRequest();
  // Remote network calls can block on a broken link. Keep
  // HTTP responsive and command cadence deterministic while CAN is active.
  // Never start MQTT/OVMS connect work while a firmware upload is running:
  // a broker connect blocks the loop for seconds, which starves the HTTP
  // server task and aborts the multipart upload mid-transfer.
  if (activeSequence == CanSequence::NONE && !otaInProgress) {
    manageMQTT();
    manageOvmsV2();
  }
  modemTick();
  if (wifiEnabled && WiFi.status() == WL_CONNECTED) {
    if (!arduinoOtaBegun) setupArduinoOTA();
    if (arduinoOtaBegun) ArduinoOTA.handle();
  }
  if (!otaInProgress) {
    canMaintainBus();
    readAndHandleCANMessage();
    manageEvCAN();
    manageBmsDiagnostics(millis());
    manageVinQuery(millis());
    manageVinObdProbe(millis());
    CanSeqResult seqRes = manageCANSequence(millis());
    if (seqRes == CanSeqResult::WAKE_SUCCESS) {
        Serial.println("### Car is awake! ###");
        bmsRequestImmediateBurst();
    } else if (seqRes == CanSeqResult::WAKE_TIMEOUT) {
        Serial.println("### Car wake timeout! ###");
    } else if (seqRes == CanSeqResult::SEQUENCE_FINISHED) {
        Serial.println("### Sequence Complete ###");
        // The gateway only answers diagnostics for a few seconds after the
        // command that woke it, so do not wait for the 5 s activity sample.
        bmsRequestImmediateBurst();
    }
    if (hvacTimerEndsMs != 0 && (long)(millis() - hvacTimerEndsMs) >= 0) {
      hvacTimerEndsMs = 0;
      hvacTimerStopPending = true;
      mqttUpdateHvacTimer(0);
    }
    uint16_t remainingMinutes = hvacTimerRemainingMinutes();
    if (remainingMinutes != lastPublishedHvacMinutes) {
      lastPublishedHvacMinutes = remainingMinutes;
      mqttUpdateHvacTimer(remainingMinutes);
    }
    if (hvacTimerStopPending && activeSequence == CanSequence::NONE &&
        startSequence(CanSequence::HVAC_OFF, millis())) {
      hvacTimerStopPending = false;
      Serial.println("### HVAC timer expired: OFF command started ###");
    }
    // Sustain a running remote climate session (OEM TCU heartbeat). Bounded by
    // the user-requested timer so the 12 V battery is never drained forever.
    // Start/keep the heartbeat from the accepted remote request itself. Do
    // not wait for 0x54B to confirm HVAC ON: the BCM can still be in its
    // starting state, and without heartbeat it drops the request.
    manageHvacSessionKeepalive(millis(), hvacTimerEndsMs != 0);
  }
  manageWiFi();
  processSerialInput();

  // --- Power save: deep sleep after idle period ---
  // Never sleep while charging or running remote climate
  bool callControlKeepsAwake = getModemEnable() && getModemCallEnabled();
  if (getSleepEnabled() && !callControlKeepsAwake && !canLoggerIsRecording() &&
      !otaInProgress && activeSequence == CanSequence::NONE &&
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
