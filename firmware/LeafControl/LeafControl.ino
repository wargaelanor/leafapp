#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <NetBIOS.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <sys/time.h>
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
#include "tripLog.h"
#include "modemProbe.h"
#include "boardConfig.h"
#include <Preferences.h>

#if defined(LEAF_BOARD_TCALL) && (LEAF_BOARD_TCALL)
#define LEAF_FIRMWARE_VERSION "FW-2026.11.08.125-TCALL"
#define LEAF_FIRMWARE_DESCRIPTION "T-Call: two MCP2515 CAN buses + SIM800"
#else
#define LEAF_FIRMWARE_VERSION "FW-2026.11.08.125-CMD"
#define LEAF_FIRMWARE_DESCRIPTION "T-2CAN: stable scroll, Wi-Fi scan"
#endif

WebServer server(80);
DNSServer dnsServer;
bool captivePortalActive = false;
unsigned long rebootPendingAt = 0;
static uint8_t rebootPendingReason = 0;
RTC_DATA_ATTR static uint8_t lastSoftwareRestart = 0;

// The reason must survive the reset it describes. RTC memory was not reliable
// for this on a software reset, so it is kept in NVS and consumed on next boot.
static void recordSoftwareRestart(uint8_t reason) {
  lastSoftwareRestart = reason;
  Preferences prefs;
  if (prefs.begin("leafboot", false)) {
    prefs.putUChar("why", reason);
    prefs.end();
  }
}

static void loadSoftwareRestart() {
  Preferences prefs;
  if (!prefs.begin("leafboot", false)) return;
  lastSoftwareRestart = prefs.getUChar("why", 0);
  prefs.putUChar("why", 0);
  prefs.end();
}
// Largest gap between loop() iterations: proves whether a stalled HTTP
// response comes from a blocked main loop or only from the network.
static uint32_t maxLoopGapMs = 0;
static uint32_t lastLoopTickMs = 0;
static uint32_t developerSession = 0;
static unsigned long developerSessionSeenMs = 0;
static const unsigned long DEVELOPER_SESSION_MS = 15UL * 60UL * 1000UL;
static uint8_t currentWifiProfile = 0;
static volatile int lastWifiDisconnectReason = 0;
// Set once NTP was started after the first successful Wi-Fi connection.
static bool timeSyncStarted = false;
// Last time someone opened a page or sent a command. Used to stop a forgotten
// phone on the board's AP from blocking STA retries forever.
static unsigned long lastUserActivityMs = 0;
// How many times the radio was restarted without getting a link back.
static uint8_t wifiRadioRestarts = 0;
// mDNS/NBNS state. They are also useful on the board's own access point, where
// there is no STA link at all, so they are started once the radio is up.
static bool mdnsUp = false;
// Milliseconds since the STA link went down (0 while connected). Reported as
// wifi_down_s so a "board alive but unreachable" state is visible at a glance.
static unsigned long wifiStaDownSinceMs = 0;

// (Re)create the configuration AP. Used by setup and by the Wi-Fi recovery
// path, which restarts the whole radio and therefore loses the AP config.
static void startSoftAp() {
  const char* ssid = getApSSID();
  const char* pass = getApPassword();
  if (strlen(ssid) == 0) {
    // Recovery AP: the saved STA password may be wrong, so the settings page
    // must remain reachable. It is open only in this fallback case.
    ssid = "LeafControl-Setup";
    pass = "";
  }
  bool ok;
  WiFi.setTxPower(WIFI_POWER_11dBm);
  if (strlen(pass) >= 8) {
    ok = WiFi.softAP(ssid, pass);
  } else {
    ok = WiFi.softAP(ssid);
  }
  Serial.printf("softAP(%s)=%s ip=%s\n", ssid, ok ? "OK" : "FAIL",
                WiFi.softAPIP().toString().c_str());
}

// Serve leaf.local over whichever interface is up. ESPmDNS answers on the AP
// interface too, so connecting straight to the board's access point works
// without knowing its IP. Idempotent: safe to call from several places.
static void startMdnsServices() {
  if (mdnsUp) return;
  if (!MDNS.begin(getHostName())) {
    Serial.println("Error starting mDNS service!");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  mdnsUp = true;
  Serial.println("mDNS service started.");
  if (!NBNS.begin(getHostName())) {
    Serial.println("Error starting NBNS service!");
  } else {
    Serial.println("NBNS service started.");
  }
}

static void stopMdnsServices() {
  if (!mdnsUp) return;
  MDNS.end();
  NBNS.end();
  mdnsUp = false;
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

// Deferred-publish state for the throttled EV-CAN handlers: a frame captured
// inside a throttle window sets the pending flag and managePendingPublishes()
// sends the latest cached value once the window expires.
static bool packPubPending = false;
static unsigned long packPubLastMs = 0;
static bool gidsPubPending = false;
static unsigned long gidsPubLastMs = 0;
static bool ambientPubPending = false;
static unsigned long ambientPubLastMs = 0;
void managePendingPublishes();
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
  uint8_t cachedDoors;        // bit0 FL, bit1 FR, bit2 RL, bit3 RR, bit4 trunk, bit5 headlights
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
  lastUserActivityMs = millis();
  noteCarActivity();
}

// Restore cached state after deep sleep wake
void restoreFromRtc() {
  if (rtcState.magic != 0xA71EE503) return;
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
  doorFL = rtcState.cachedDoors & 0x01;
  doorFR = rtcState.cachedDoors & 0x02;
  doorRL = rtcState.cachedDoors & 0x04;
  doorRR = rtcState.cachedDoors & 0x08;
  doorTrunk = rtcState.cachedDoors & 0x10;
  headlightsState = rtcState.cachedDoors & 0x20;

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
  mqttUpdateDoors(doorFL, doorFR, doorRL, doorRR, doorTrunk);
  mqttUpdateHeadlights(headlightsState);
}

// Save current state to RTC memory
void saveToRtc() {
  rtcState.magic = 0xA71EE503;
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
  rtcState.cachedDoors = (doorFL ? 0x01 : 0x00)
                       | (doorFR ? 0x02 : 0x00)
                       | (doorRL ? 0x04 : 0x00)
                       | (doorRR ? 0x08 : 0x00)
                       | (doorTrunk ? 0x10 : 0x00)
                       | (headlightsState ? 0x20 : 0x00);
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

// Lightweight page for a poor link: a few kilobytes, no images, no scripts
// beyond the polling loop. Also the fallback entry point from the OTA page.
void handleLite() {
  noteUserActivity();
  NetworkClient& client = server.client();
  client.setTimeout(2000);
  String accept = server.header("Accept-Encoding");
  accept.toLowerCase();
  bool useGzip = accept.indexOf("gzip") >= 0;
  const unsigned char* body = useGzip ? LITE_HTML_GZ : (const unsigned char*)LITE_HTML;
  const size_t len = useGzip ? LITE_HTML_GZ_LEN : sizeof(LITE_HTML) - 1;
  server.setContentLength(len);
  server.sendHeader("Cache-Control", "no-store");
  if (useGzip) server.sendHeader("Content-Encoding", "gzip");
  server.send(200, "text/html; charset=utf-8", "");
  for (size_t off = 0; off < len; off += 1024) {
    if (!client.connected()) break;
    size_t chunk = len - off;
    if (chunk > 1024) chunk = 1024;
    if (client.write_P((const char*)body + off, chunk) != chunk) break;
  }
  client.stop();
}

// Root page: the light one by default. A fresh board must open instantly even
// over a poor link, and the full app is one tap away.
// Seconds since the Unix epoch. NTP is configured only after the board joins a
// router (STA), so AP-only boards have no wall clock. Keep the last known time
// in NVS, advance it with millis(), and accept the phone's clock via /api/time.
static Preferences clockPrefs;
static uint32_t clockBaseEpoch = 0;
static unsigned long clockBaseMs = 0;
static unsigned long clockSavedMs = 0;

static void clockPersist(uint32_t epoch) {
  clockBaseEpoch = epoch;
  clockBaseMs = millis();
  clockSavedMs = millis();
  if (!clockPrefs.begin("leafclock", false)) return;
  clockPrefs.putULong("e", epoch);
  clockPrefs.end();
}

void setBoardEpoch(uint32_t epoch) {
  if (epoch < 1600000000UL) return;
  struct timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  clockPersist(epoch);
  Serial.printf("[time] clock set to %lu\n", (unsigned long)epoch);
}

// Restore the stored epoch once, early in setup. Without this tripLog's
// time(nullptr) stays 0 and no trip/charge record is ever started.
void clockRestore() {
  if (clockBaseEpoch) return;
  if (!clockPrefs.begin("leafclock", false)) return;
  uint32_t stored = clockPrefs.getULong("e", 0);
  clockPrefs.end();
  if (stored < 1600000000UL) return;
  uint32_t restored = stored + millis() / 1000UL;
  setBoardEpoch(restored);
  Serial.println("[time] restored from NVS (approximate, awaiting NTP/phone)");
}

unsigned long boardEpoch() {
  time_t now = time(nullptr);
  if (now > 1600000000) {
    uint32_t real = (uint32_t)now;
    // Persist at most once per ten minutes; NVS wears out otherwise.
    if (millis() - clockSavedMs > 600000UL) clockPersist(real);
    return real;
  }
  if (!clockBaseEpoch) return 0;
  return clockBaseEpoch + (millis() - clockBaseMs) / 1000UL;
}

// The Android app knows the real time and pushes it when the board reports 0.
void handleApiTime() {
  uint32_t epoch = server.hasArg("epoch") ? (uint32_t)server.arg("epoch").toInt() : 0;
  if (epoch < 1600000000UL) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  setBoardEpoch(epoch);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRootEntry() {
  if (getUiLite()) handleLite(); else handleRoot();
}

// /ui?mode=lite|full — remembers the choice in NVS so it survives a reboot.
void handleUiSwitch() {
  bool lite = server.arg("mode") != "full";
  setUiLite(lite);
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleRoot() {
  noteUserActivity();
  // A disconnected client must not hold the single-threaded WebServer for
  // 5 seconds per chunk. Abort this response on the first short write.
  // The page is sent gzipped when the browser accepts it: over the board's own
  // access point the uncompressed 100 KB took minutes, the gzip copy is ~25 KB.
  NetworkClient& client = server.client();
  client.setTimeout(2000);
  String accept = server.header("Accept-Encoding");
  accept.toLowerCase();
  bool useGzip = accept.indexOf("gzip") >= 0;
  const unsigned char* body = useGzip ? INDEX_HTML_GZ : (const unsigned char*)INDEX_HTML;
  const size_t htmlLen = useGzip ? INDEX_HTML_GZ_LEN : sizeof(INDEX_HTML) - 1;
  server.setContentLength(htmlLen);
  server.sendHeader("Vary", "Accept-Encoding");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  if (useGzip) server.sendHeader("Content-Encoding", "gzip");
  server.send(200, "text/html; charset=utf-8", "");
  size_t chunkIndex = 0;
  for (size_t offset = 0; offset < htmlLen; offset += 2048) {
    if (!client.connected()) break;
    size_t chunk = htmlLen - offset;
    if (chunk > 2048) chunk = 2048;
    if (client.write_P((const char*)body + offset, chunk) != chunk) break;
    if ((++chunkIndex & 3) == 0) delay(0);   // yield every 8 KB
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
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
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
  for (size_t i = 0; i < WEBAPP_ASSET_COUNT; i++) {
    if (path == WEBAPP_ASSETS[i].path) {
      streamProgmemImage(WEBAPP_ASSETS[i].data, WEBAPP_ASSETS[i].len, WEBAPP_ASSETS[i].type);
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

// Scan 2.4 GHz networks from the settings page. The phone keyboard is the
// usual source of a wrong SSID/password, so show what the board actually sees.
void handleWifiScan() {
  noteUserActivity();
  int n = WiFi.scanNetworks(false, true);
  String json = "[";
  int added = 0;
  for (int i = 0; i < n && added < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    if (added) json += ",";
    json += "{\"ssid\":\"";
    for (size_t k = 0; k < ssid.length(); k++) {
      char c = ssid[k];
      if (c == '"' || c == '\\') json += '\\';
      if (c >= 32) json += c;
    }
    json += "\",\"rssi\":";
    json += String(WiFi.RSSI(i));
    json += ",\"enc\":";
    json += String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1);
    json += "}";
    added++;
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
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
    "{\"fw\":\"%s\",\"fw_desc\":\"%s\",\"board\":\"%s\",\"soc\":%.1f,\"cabin\":%.1f,\"charging\":%d,\"charger\":\"%s\",\"hvac\":%d,"
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
    "\"modem\":\"%s\",\"uptime\":%lu,\"epoch\":%lu,\"free_heap\":%lu,\"psram\":%u,\"psram_free\":%u,\"reset_reason\":%d,\"software_restart\":%u,"
    "\"remote_mode\":%d,\"ui_lite\":%d,\"mqtt_connected\":%d,\"ovms_v2_connected\":%d,\"ovms_v2_paranoid\":%d,"
    "\"wifi_status\":%d,\"wifi_rssi\":%d,\"wifi_profile\":%u,\"wifi_disconnect\":%d,"
    "\"wifi_down_s\":%lu,"
    "\"ap_clients\":%u,"
    "\"trips\":%u,\"charges\":%u,\"driving\":%d,"
    "\"night_from\":%u,\"night_to\":%u,\"tariff_day\":%.2f,\"tariff_night\":%.2f,"
    "\"sleep_on\":%d,\"sleep_after_h\":%d,\"wake_fast_min\":%d,\"idle_s\":%lu,"
    "\"gps\":{\"lat\":%.5f,\"lon\":%.5f,\"age\":%lu}}",
    LEAF_FIRMWARE_VERSION, LEAF_FIRMWARE_DESCRIPTION, BOARD_NAME,
    currentSOC, cabinTemp, (isChargingNow || tripLogIsCharging()) ? 1 : 0, chargerStateStr(currentChargerState),
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
    modemGetStateStr(), (unsigned long)(millis() / 1000UL), (unsigned long)boardEpoch(),
    (unsigned long)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
    (int)esp_reset_reason(), (unsigned int)lastSoftwareRestart,
    (int)getRemoteTransport(), getUiLite() ? 1 : 0, mqttIsConnected() ? 1 : 0, ovmsV2IsConnected() ? 1 : 0,
    ovmsV2ParanoidIsActive() ? 1 : 0,
    (int)WiFi.status(), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
    (unsigned int)(currentWifiProfile + 1), lastWifiDisconnectReason,
    (unsigned long)(wifiStaDownSinceMs ? (millis() - wifiStaDownSinceMs) / 1000UL : 0UL),
    (unsigned)WiFi.softAPgetStationNum(),
    (unsigned)tripLogTripsCount(), (unsigned)tripLogChargesCount(),
    (evData.vehicleOn == 1 || tripLogIsDriving() || fabsf(evData.packPower) > 1.0f) ? 1 : 0,
    (unsigned)tripLogNightFrom(), (unsigned)tripLogNightTo(),
    tripLogTariffDay(), tripLogTariffNight(),
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

// Same probe from the diagnostics page: it is a read-only OBD-II request, so it
// does not need the developer password, only a POST.
void handleVinProbePublic() {
  noteUserActivity();
  requestVinObdProbe();
  server.send(200, "application/json", "{\"ok\":true,\"probe\":\"obd09_02\"}");
}

// --- history kept on the board -------------------------------------------
// Reads are harmless GETs. Writes (clearing history, rebooting) require the
// X-Requested-With header, which a cross-site form cannot set.
static bool sameOriginPost() {
  return server.method() == HTTP_POST && server.hasHeader("X-Requested-With");
}

void handleTripsList() {
  noteUserActivity();
  server.sendHeader("Cache-Control", "no-store");
  tripLogSendTrips();
}

void handleChargesList() {
  noteUserActivity();
  server.sendHeader("Cache-Control", "no-store");
  tripLogSendCharges();
}

void handleTripsClear() {
  if (!sameOriginPost()) {
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  noteUserActivity();
  tripLogClearTrips();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleChargesClear() {
  if (!sameOriginPost()) {
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  noteUserActivity();
  tripLogClearCharges();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Night window used by the board when it splits trip/charge energy. The app
// sends its tariff window here so the board and the phone agree.
void handleTariffSet() {
  if (!sameOriginPost()) {
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  int from = server.arg("from").toInt();
  int to = server.arg("to").toInt();
  if (from < 0 || from > 23 || to < 0 || to > 23) {
    server.send(400, "application/json", "{\"error\":\"bad_hours\"}");
    return;
  }
  double tariffDay = server.arg("day").toFloat();
  double tariffNight = server.arg("night").toFloat();
  if (!server.hasArg("day")) tariffDay = tripLogTariffDay();
  if (!server.hasArg("night")) tariffNight = tripLogTariffNight();
  noteUserActivity();
  tripLogSetTariff(tariffDay, tariffNight, (uint8_t)from, (uint8_t)to);
  server.send(200, "application/json", "{\"ok\":true}");
}

// Hidden maintenance button in the settings page. It needs a deliberate
// 3-second hold in the UI; the delay here lets the HTTP answer leave first.
void handleReboot() {
  if (!sameOriginPost()) {
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  noteUserActivity();
  Serial.println("### Reboot requested from settings page ###");
  rebootPendingAt = millis() + 1500;
  rebootPendingReason = 6;
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
}

// Modem bench test: scans baud rates and prints the AT answers. Read-only from
// the car's point of view, so no developer password; it only opens Serial2.
void handleModemProbe() {
  noteUserActivity();
  ModemState st = modemGetState();
  if (getModemEnable() && st != ModemState::OFF && st != ModemState::FAILED) {
    server.send(409, "application/json", "{\"error\":\"modem_busy\"}");
    return;
  }
  String body = modemProbeJson();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleModemPower() {
  if (!sameOriginPost()) {
    server.send(403, "application/json", "{\"error\":\"forbidden\"}");
    return;
  }
  noteUserActivity();
  modemProbePowerPulse();
  server.send(200, "application/json", "{\"ok\":true,\"pwrkey\":true}");
}

// Live CAN watch: one JSON row per identifier, updated in place on the board.
// The page polls this while the developer monitor is open.
void handleCanWatch() {
  if (!requireDeveloperAuth()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char buf[192];
  server.sendContent("[");
  uint16_t count = canWatchCount();
  for (uint16_t i = 0; i < count; i++) {
    CanWatchRow row;
    if (!canWatchGet(i, &row)) continue;
    char hex[26];
    int pos = 0;
    for (uint8_t b = 0; b < row.dlc && pos < (int)sizeof(hex) - 3; b++) {
      pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", row.data[b]);
    }
    if (pos > 0) hex[pos - 1] = '\0'; else hex[0] = '\0';
    snprintf(buf, sizeof(buf),
             "%s{\"b\":\"%c\",\"id\":%lu,\"dlc\":%u,\"d\":\"%s\",\"n\":%lu,\"ms\":%lu}",
             i ? "," : "", row.bus, (unsigned long)row.id, (unsigned)row.dlc, hex,
             (unsigned long)row.changes, (unsigned long)row.ageMs);
    server.sendContent(buf);
  }
  server.sendContent("]");
  server.sendContent("");
}

void handleCanWatchReset() {
  if (!requireDeveloperAuth()) return;
  canWatchReset();
  server.send(200, "application/json", "{\"ok\":true}");
}

// --- developer: armed frame replay ---------------------------------------
// Saved commands are stored in the browser. The board only sends them while
// the user explicitly armed replay for ten minutes, and only on CAR-CAN.
static unsigned long replayArmedUntilMs = 0;

static bool replayArmed() {
  return replayArmedUntilMs != 0 && (int32_t)(replayArmedUntilMs - millis()) > 0;
}

void handleReplayArm() {
  if (!requireDeveloperAuth()) return;
  replayArmedUntilMs = millis() + 10UL * 60UL * 1000UL;
  server.send(200, "application/json", "{\"ok\":true,\"minutes\":10}");
}

void handleReplayDisarm() {
  if (!requireDeveloperAuth()) return;
  replayArmedUntilMs = 0;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleReplaySend() {
  if (!requireDeveloperAuth()) return;
  if (!replayArmed()) {
    server.send(403, "application/json", "{\"error\":\"not_armed\"}");
    return;
  }
  if (!server.hasArg("id") || !server.hasArg("d")) {
    server.send(400, "application/json", "{\"error\":\"bad_request\"}");
    return;
  }
  uint32_t id = strtoul(server.arg("id").c_str(), nullptr, 16);
  if (id == 0 || id > 0x7FF) {
    server.send(400, "application/json", "{\"error\":\"bad_id\"}");
    return;
  }
  String hex = server.arg("d");
  hex.replace(" ", "");
  uint8_t data[8];
  uint8_t len = 0;
  for (size_t i = 0; i + 1 < hex.length() && len < 8; i += 2) {
    data[len++] = (uint8_t)strtoul(hex.substring(i, i + 2).c_str(), nullptr, 16);
  }
  if (len == 0) {
    server.send(400, "application/json", "{\"error\":\"empty\"}");
    return;
  }
  uint8_t repeats = 1;
  if (server.hasArg("n")) {
    long v = server.arg("n").toInt();
    if (v > 0 && v <= 20) repeats = (uint8_t)v;
  }
  uint16_t gap = 100;
  if (server.hasArg("gap")) {
    long v = server.arg("gap").toInt();
    if (v >= 20 && v <= 500) gap = (uint16_t)v;
  }
  uint8_t sent = 0;
  for (uint8_t i = 0; i < repeats; i++) {
    if (sendCANForced(id, data, len)) sent++;
    if (i + 1 < repeats) delay(gap);
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"sent\":%u,\"repeats\":%u,\"id\":\"0x%03lX\"}",
           (unsigned)sent, (unsigned)repeats, (unsigned long)id);
  server.send(200, "application/json", buf);
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
        recordSoftwareRestart(3);
        delay(100);
        ESP.restart();
      }
      continue;
    }
    uint32_t heartbeat = loopHeartbeatMs;
    if (heartbeat != 0 && millis() - heartbeat > 60000UL) {
      Serial.println("### Supervisor: main loop stalled, rebooting ###");
      recordSoftwareRestart(1);
      delay(100);
      ESP.restart();
    }
    // Heap floor: a normal run keeps ~140 KB free, so a drop under 25 KB means
    // fragmentation or a leak that will start failing allocations. Rebooting
    // here is cheaper than a board that silently stops answering.
    uint32_t freeHeap = ESP.getFreeHeap();
    static uint8_t lowHeapSamples = 0;
    if (freeHeap < 25000UL) {
      if (++lowHeapSamples >= 3) {
        Serial.printf("### Supervisor: free heap %u, rebooting ###\n", (unsigned)freeHeap);
        recordSoftwareRestart(5);
        delay(100);
        ESP.restart();
      }
    } else {
      lowHeapSamples = 0;
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
    "<p style='color:#666;font-size:13px;max-width:340px;margin:14px auto'>Если основная страница открывается медленно, "
    "есть упрощённая: <a href='/lite'>/lite</a> — несколько килобайт, только цифры и четыре команды.</p>"
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

  // Tariff lives on the board so it survives a different browser/origin.
  if (server.hasArg("tariff_day") || server.hasArg("night_from")) {
    tripLogSetTariff(
        server.arg("tariff_day").toFloat(),
        server.arg("tariff_night").toFloat(),
        (uint8_t)server.arg("night_from").toInt(),
        (uint8_t)server.arg("night_to").toInt());
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

// --- JSON config API for the native app -------------------------------------
// The web form is the only writer of settings so far; the Android app needs
// the same fields without a browser. GET returns the current values (passwords
// only as *_set booleans), POST accepts any subset of them. Semantics mirror
// handleSettingsSave: empty password = keep, empty wifi_ssid = STA off, and a
// deferred reboot 2 s later so Wi-Fi/AP changes take effect.

// Minimal flat-JSON reader: finds "key" and copies its value (string, number,
// bool) into out. No nesting is used by this API.
static bool jsonFindValue(const String& body, const char* key, String& out) {
  String needle = String("\"") + key + "\"";
  int pos = 0;
  while ((pos = body.indexOf(needle, pos)) >= 0) {
    int i = pos + needle.length();
    while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
    if (i >= (int)body.length() || body[i] != ':') { pos += 1; continue; }
    i++;
    while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
    if (i >= (int)body.length()) return false;
    out = "";
    if (body[i] == '"') {
      i++;
      while (i < (int)body.length()) {
        char c = body[i];
        if (c == '\\' && i + 1 < (int)body.length()) {
          char n = body[i + 1];
          out += (n == 'n') ? '\n' : (n == 't') ? '\t' : n;
          i += 2;
          continue;
        }
        if (c == '"') { return true; }
        out += c;
        i++;
      }
      return false;  // unterminated string
    }
    while (i < (int)body.length() && body[i] != ',' && body[i] != '}') {
      out += body[i];
      i++;
    }
    out.trim();
    return true;
  }
  return false;
}

static bool jsonHas(const String& body, const char* key) {
  String v;
  return jsonFindValue(body, key, v);
}

static bool jsonStr(const String& body, const char* key, String& out) {
  return jsonFindValue(body, key, out);
}

static bool jsonBool(const String& body, const char* key, bool& out) {
  String v;
  if (!jsonFindValue(body, key, v)) return false;
  out = (v == "true" || v == "1");
  return true;
}

static bool jsonInt(const String& body, const char* key, long& out) {
  String v;
  if (!jsonFindValue(body, key, v)) return false;
  out = strtol(v.c_str(), nullptr, 10);
  return true;
}

static bool jsonFloat(const String& body, const char* key, float& out) {
  String v;
  if (!jsonFindValue(body, key, v)) return false;
  out = strtof(v.c_str(), nullptr);
  return true;
}

void handleApiConfigGet() {
  String out;
  out.reserve(900);
  out += "{\"fw\":\""; out += LEAF_FIRMWARE_VERSION;
  out += "\",\"board\":\""; out += BOARD_NAME;
  out += "\",\"remote_mode\":"; out += String((int)getRemoteTransport());
  out += ",\"mqtt_server\":\""; out += getMqttServer();
  out += "\",\"mqtt_port\":"; out += String(getMqttPort());
  out += ",\"mqtt_user\":\""; out += getMqttUser();
  out += "\",\"mqtt_password_set\":"; out += strlen(getMqttPassword()) > 0 ? "true" : "false";
  out += ",\"mqtt_tls\":"; out += getMqttTls() ? "true" : "false";
  out += ",\"hass_discovery\":"; out += getHassDiscoveryEnabled() ? "true" : "false";
  out += ",\"mqtt_vehicle_id\":\""; out += getMqttVehicleId();
  out += "\",\"vehicle_id\":\""; out += getVehicleId();
  out += "\",\"mqtt_connected\":"; out += mqttIsConnected() ? "1" : "0";
  out += ",\"wifi_ssid\":\""; out += getWifiSSID();
  out += "\",\"wifi_password_set\":"; out += strlen(getWifiPassword()) > 0 ? "true" : "false";
  out += ",\"wifi_ssid2\":\""; out += getWifiSSID2();
  out += "\",\"wifi_password2_set\":"; out += strlen(getWifiPassword2()) > 0 ? "true" : "false";
  out += ",\"wifi_ssid3\":\""; out += getWifiSSID3();
  out += "\",\"wifi_password3_set\":"; out += strlen(getWifiPassword3()) > 0 ? "true" : "false";
  out += ",\"ap_ssid\":\""; out += getApSSID();
  out += "\",\"ap_password_set\":"; out += strlen(getApPassword()) > 0 ? "true" : "false";
  out += ",\"host_name\":\""; out += getHostName();
  out += "\",\"locking_enabled\":"; out += getLockingEnabled() ? "true" : "false";
  out += ",\"sleep_enable\":"; out += getSleepEnabled() ? "true" : "false";
  out += ",\"sleep_after_h\":"; out += String(getSleepAfterHours());
  out += ",\"wake_fast_min\":"; out += String(getWakeFastMinutes());
  out += ",\"wake_slow_min\":"; out += String(getWakeSlowMinutes());
  out += ",\"tariff_day\":"; out += String(tripLogTariffDay(), 2);
  out += ",\"tariff_night\":"; out += String(tripLogTariffNight(), 2);
  out += ",\"night_from\":"; out += String((int)tripLogNightFrom());
  out += ",\"night_to\":"; out += String((int)tripLogNightTo());
  out += ",\"ui_lite\":"; out += getUiLite() ? "true" : "false";
  out += ",\"modem_enable\":"; out += getModemEnable() ? "true" : "false";
  out += ",\"ovms_server\":\""; out += getOvmsServer();
  out += "\",\"ovms_port\":"; out += String(getOvmsPort());
  out += ",\"ovms_tls\":"; out += getOvmsTls() ? "true" : "false";
  out += ",\"ovms_password_set\":"; out += strlen(getOvmsPassword()) > 0 ? "true" : "false";
  out += ",\"ovms_module_password_set\":"; out += strlen(getOvmsModulePassword()) > 0 ? "true" : "false";
  out += "}";
  // Self-check: this JSON is built by hand, and a single lost quote (as in
  // .122: missing \" before mqtt_connected) breaks every client parser while
  // the endpoint still returns 200. An odd quote count means malformed —
  // answer with a small valid object instead of poisoning the app.
  {
    int quotes = 0;
    for (size_t i = 0; i < out.length(); i++) {
      if (out[i] == '"') quotes++;
    }
    if (quotes % 2 != 0) {
      Serial.printf("### /api/config JSON malformed (%d quotes) - sending fallback ###\n", quotes);
      out = "{\"fw\":\"";
      out += LEAF_FIRMWARE_VERSION;
      out += "\",\"error\":\"config_json_malformed\"}";
    }
  }
  server.send(200, "application/json", out);
}

void handleApiConfigSave() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"no_body\"}");
    return;
  }
  String body = server.arg("plain");
  Serial.println("### Settings saved via app JSON API ###");
  bool noReboot = false;
  jsonBool(body, "no_reboot", noReboot);  // optional: apply without restarting

  String s;
  long n;
  bool b;
  float f;

  if (jsonHas(body, "remote_mode") && jsonInt(body, "remote_mode", n) && n >= 0 && n <= 2) {
    setRemoteTransport(static_cast<RemoteTransport>(n));
  }
  if (jsonStr(body, "mqtt_server", s)) { s.trim(); setMqttServer(s.c_str()); }
  if (jsonInt(body, "mqtt_port", n) && n >= 1 && n <= 65535) setMqttPort((int)n);
  if (jsonStr(body, "mqtt_user", s)) { s.trim(); setMqttUser(s.c_str()); }
  if (jsonStr(body, "mqtt_password", s) && s.length() > 0) setMqttPassword(s.c_str());
  if (jsonStr(body, "vehicle_id", s)) { s.trim(); if (s.length() > 0) setVehicleId(s.c_str()); }
  if (jsonStr(body, "mqtt_vehicle_id", s)) { s.trim(); if (s.length() > 0) setMqttVehicleId(s.c_str()); }
  if (jsonBool(body, "mqtt_tls", b)) setMqttTls(b);
  if (jsonBool(body, "hass_discovery", b)) setHassDiscoveryEnabled(b);

  if (jsonStr(body, "wifi_ssid", s)) { s.trim(); setWifiSSID(s.c_str()); }
  if (jsonStr(body, "wifi_password", s) && s.length() > 0) setWifiPassword(s.c_str());
  if (jsonStr(body, "wifi_ssid2", s)) { s.trim(); setWifiSSID2(s.c_str()); }
  if (jsonStr(body, "wifi_password2", s) && s.length() > 0) setWifiPassword2(s.c_str());
  if (jsonStr(body, "wifi_ssid3", s)) { s.trim(); setWifiSSID3(s.c_str()); }
  if (jsonStr(body, "wifi_password3", s) && s.length() > 0) setWifiPassword3(s.c_str());

  if (jsonStr(body, "ap_ssid", s)) { s.trim(); setApSSID(s.c_str()); }
  if (jsonStr(body, "ap_password", s) && s.length() > 0) {
    if (setApPassword(s.c_str()) != ConfigStatus::OK) {
      Serial.println("AP password rejected (must be >= 8 chars), keeping previous");
    }
  }
  if (jsonStr(body, "host_name", s)) { s.trim(); if (s.length() > 0) setHostName(s.c_str()); }

  if (jsonBool(body, "locking_enabled", b)) setLockingEnabled(b);
  if (jsonBool(body, "ui_lite", b)) setUiLite(b);

  if (jsonBool(body, "sleep_enable", b)) setSleepEnabled(b);
  if (jsonInt(body, "sleep_after_h", n) && n >= 1 && n <= 168) setSleepAfterHours((int)n);
  if (jsonInt(body, "wake_fast_min", n) && n >= 1 && n <= 1440) setWakeFastMinutes((int)n);
  if (jsonInt(body, "wake_slow_min", n) && n >= 1 && n <= 1440) setWakeSlowMinutes((int)n);

  if (jsonHas(body, "tariff_day") || jsonHas(body, "night_from")) {
    float day = (float)tripLogTariffDay(), night = (float)tripLogTariffNight();
    uint8_t from = tripLogNightFrom(), to = tripLogNightTo();
    if (jsonFloat(body, "tariff_day", f)) day = f;
    if (jsonFloat(body, "tariff_night", f)) night = f;
    if (jsonInt(body, "night_from", n) && n >= 0 && n <= 23) from = (uint8_t)n;
    if (jsonInt(body, "night_to", n) && n >= 0 && n <= 23) to = (uint8_t)n;
    tripLogSetTariff(day, night, from, to);
  }

  if (jsonBool(body, "modem_enable", b)) setModemEnable(b);

  if (!noReboot) {
    rebootPendingAt = millis() + 2000;
    rebootPendingReason = 3;
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  } else {
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":false}");
  }
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
  static int lastPubbed = -1;
  evData.gids = gids;
  if (gids == lastPubbed) return;
  if (gidsPubLastMs != 0 && millis() - gidsPubLastMs < 30000) {
    gidsPubPending = true;
    return;
  }
  gidsPubLastMs = millis();
  gidsPubPending = false;
  lastPubbed = gids;
  mqttUpdateGids(gids);
}

void handleEvPackStatus(float voltage, float current) {
  noteCarActivity();
  static float lastPubbedV = -1.0f;
  static float lastPubbedA = 0.0f;
  evData.packVoltage = voltage;
  evData.packCurrent = current;
  evData.packPower = voltage * current / 1000.0f;
  evData.packDataValid = true;
  evData.packVoltageValid = true;
  if (voltage == lastPubbedV && current == lastPubbedA) return;
  // Fast lane while the car is doing something (driving / charging): the
  // MQTT subscribers see V/A move in ~real time. Idle: the old 5 s window.
  const bool active = (evData.vehicleOn == 1) || (fabsf(current) > 1.0f);
  const unsigned long window = active ? 1000UL : 5000UL;
  if (packPubLastMs != 0 && millis() - packPubLastMs < window) {
    packPubPending = true;  // window full: publish the LATEST value when it opens
    return;
  }
  packPubLastMs = millis();
  packPubPending = false;
  lastPubbedV = voltage;
  lastPubbedA = current;
  mqttUpdatePackStatus(voltage, current);
}

// Fires the deferred publishes scheduled by the throttled EV handlers above.
// Called from loop() so a value captured inside a throttle window is still
// sent (with the freshest cached reading) once the window expires.
void managePendingPublishes() {
  unsigned long now = millis();
  // Same dynamic window as handleEvPackStatus: 1 s while active, 5 s idle
  const unsigned long packWindow =
      (evData.vehicleOn == 1 || fabsf(evData.packCurrent) > 1.0f) ? 1000UL : 5000UL;
  if (packPubPending && evData.packDataValid &&
      now - packPubLastMs >= packWindow) {
    packPubPending = false;
    packPubLastMs = now;
    mqttUpdatePackStatus(evData.packVoltage, evData.packCurrent);
  }
  if (gidsPubPending &&
      now - gidsPubLastMs >= 30000) {
    gidsPubPending = false;
    gidsPubLastMs = now;
    mqttUpdateGids(evData.gids);
  }
  if (ambientPubPending && evData.ambientTemp > -90 &&
      now - ambientPubLastMs >= 60000) {
    ambientPubPending = false;
    ambientPubLastMs = now;
    mqttUpdateAmbient(evData.ambientTemp);
  }
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
  static float lastPubbed = -99.0f;
  evData.ambientTemp = ambientTemp;
  if (ambientTemp == lastPubbed) return;
  if (ambientPubLastMs != 0 && millis() - ambientPubLastMs < 60000) {
    ambientPubPending = true;
    return;
  }
  ambientPubLastMs = millis();
  ambientPubPending = false;
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

// Called from the history logger when EV-CAN shows charge current but the
// gateway has not reported the charge yet. Read-only wake sequence.
void requestVehicleWake() {
  if (otaInProgress) return;
  if (activeSequence == CanSequence::NONE) {
    startSequence(CanSequence::REFRESH, millis());
  }
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
    if (bestProfile < 0) {
      Serial.printf("Wi-Fi: none of the saved SSIDs visible (%d networks)\n", count);
      for (int i = 0; i < count && i < 12; i++) {
        Serial.printf("  seen: %s rssi=%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
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
  // A weak 5 V rail sags on Wi-Fi peaks (brownout reboot looks like a page
  // reload). 11 dBm is still plenty for a car parked near the house.
  WiFi.setTxPower(WIFI_POWER_11dBm);
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
      startMdnsServices();

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
      // With the AP still up, leaf.local must keep working from the phone that
      // is connected to it: do not tear mDNS down only because the STA link
      // dropped.
      if ((WiFi.getMode() & WIFI_AP) == 0) {
        Serial.println("WiFi disconnected, stopping mDNS/NBNS services...");
        stopMdnsServices();
      }
      wifiAvailable = false;
    }

    // Retrying every 5 s called WiFi.disconnect()+WiFi.begin() again while the
    // previous association was still in progress. At -90 dBm an association
    // needs longer than that, so the STA never finished and the board looked
    // alive (AP beaconing, visible from a phone) while being unreachable on
    // the network. Back off instead, and restart the radio if it stays dead.
    unsigned long interval = retryBackoffMs[retryStep > 3 ? 3 : retryStep];
    // Someone is configuring the board through its own access point. A missing
    // STA is then expected (that is why they use the AP), and hammering it
    // steals radio time from the page they are trying to open. Worse, the
    // 5-minute radio restart below would drop their connection mid-load.
    if (WiFi.softAPgetStationNum() > 0) {
      // A phone on the board's AP is expected during configuration. Pause STA
      // retries only while that page is actually being used; otherwise a
      // forgotten client would keep the board off the house Wi-Fi forever.
      if (lastUserActivityMs != 0 && millis() - lastUserActivityMs < 120000UL) {
        staDownSinceMs = currentTime;
        wifiStaDownSinceMs = 0;
        return;
      }
    }
    // After 45 s without a link, raise a recovery AP. This is the only
    // way back in when the saved password is wrong (e.g. autocapitalised on a
    // phone keyboard) and the board is not on the house network. Two minutes
    // used to feel broken: the user power-cycled the board and still waited
    // with no AP in sight. The hint flag also makes the next boot start the
    // AP immediately instead of re-waiting.
    if (currentTime - staDownSinceMs > 45000UL && (WiFi.getMode() & WIFI_AP) == 0) {
      Serial.println("WiFi: starting recovery AP");
      setWifiRecoveryHint(true);
      WiFi.mode(WIFI_AP_STA);
      WiFi.setSleep(false);
      startSoftAp();
      if (!captivePortalActive) {
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(53, "*", WiFi.softAPIP());
        captivePortalActive = true;
      }
      startMdnsServices();
    }
    if (currentTime - previousAttemptTime >= interval) {
      if (currentTime - staDownSinceMs >= 300000UL) {
        // Radio restarts did not bring the link back three times in a row:
        // this is a stuck driver state, not weak signal. A full reboot is the
        // only reliable way out, and it is much better than sitting offline
        // until someone power-cycles the car.
        if (++wifiRadioRestarts >= 3) {
          Serial.println("WiFi: 3 radio restarts without a link, rebooting");
          recordSoftwareRestart(4);
          delay(200);
          ESP.restart();
        }
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
    wifiRadioRestarts = 0;
    // The saved network is reachable again — the next reboot must not raise
    // the setup AP on the stale recovery hint.
    if (getWifiRecoveryHint()) setWifiRecoveryHint(false);
    if (!timeSyncStarted) {
      // No RTC on the board. NTP gives the real UTC epoch and the +3 h offset
      // makes localtime_r() (used for the night tariff) Moscow time.
      configTime(3 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");
      timeSyncStarted = true;
      Serial.println("NTP: time sync started");
    }

    // Rebind mDNS so the freshly connected STA interface is included too.
    if (mdnsUp) stopMdnsServices();
    startMdnsServices();

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
  loadSoftwareRestart();
#if defined(LEAF_BOARD_TCALL) && (LEAF_BOARD_TCALL) && (BOARD_HAS_MODEM == 0)
  // CAN is wired to the SIM800 UART pins in this profile; keep the modem off.
  pinMode(BOARD_MODEM_POWER_ON, OUTPUT);
  digitalWrite(BOARD_MODEM_POWER_ON, LOW);
  pinMode(BOARD_MODEM_PWRKEY, OUTPUT);
  digitalWrite(BOARD_MODEM_PWRKEY, HIGH);
#endif
  // Arduino loopTask runs on core 1. Keep recovery on core 0 so a blocking
  // HTTP/network call cannot also starve the task responsible for rebooting.
  BaseType_t supervisorResult = xTaskCreatePinnedToCore(
      loopSupervisorTask, "loop-watch", 3072, nullptr, 1, nullptr, 0);
  if (supervisorResult != pdPASS) {
    Serial.println("### ERROR: failed to start cross-core loop supervisor ###");
  }
  Serial.printf("### Leaf Control %s: %s ###\n", LEAF_FIRMWARE_VERSION, LEAF_FIRMWARE_DESCRIPTION);
  Serial.printf("### Board: %s, PSRAM: %u bytes, flash: %u bytes ###\n",
                BOARD_NAME, (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFlashChipSize());
  WiFi.onEvent(wifiEvent);
  initConfiguration();
  clockRestore();
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
      if (getWifiRecoveryHint()) {
        // The previous session ended with the saved network unreachable
        // (recovery AP). Raise the setup AP right at boot: making the user
        // wait through the 45 s timeout after every power cycle felt broken.
        Serial.println("WiFi: recovery hint - starting setup AP at boot");
        WiFi.mode(WIFI_AP_STA);
        WiFi.setSleep(false);
        WiFi.softAP("LeafControl-Setup");
        WiFi.setAutoReconnect(false);
        connectWifiProfile();
        WiFi.setSleep(false);
      } else {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false);
        connectWifiProfile();
        WiFi.setSleep(false);
      }
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
  // Hold every MCP2515 CS line HIGH before the first SPI transaction.
  // setupEvCAN() runs first and the other modules' CS pins are still floating
  // inputs at that point: a floating CS lets a not-yet-initialised controller
  // drive the shared MISO and corrupt the active chip's reads (EV-CAN passed
  // or failed randomly between boots for exactly this reason).
#if defined(BOARD_CAR_CAN_MCP2515) && BOARD_CAR_CAN_MCP2515
  pinMode(BOARD_CAR_MCP_CS, OUTPUT);     digitalWrite(BOARD_CAR_MCP_CS, HIGH);
  pinMode(BOARD_EVCAN_SPI_CS, OUTPUT);   digitalWrite(BOARD_EVCAN_SPI_CS, HIGH);
  pinMode(BOARD_AUXCAN_SPI_CS, OUTPUT);  digitalWrite(BOARD_AUXCAN_SPI_CS, HIGH);
#endif
  // EV-CAN first: its MISO probe briefly reconfigures GPIO35, so the CAR-CAN
  // driver must be installed afterwards.
  setupEvCAN();
  setupCAN();
  tripLogInit();

  // leaf.local must also work when the phone is attached to the board's own
  // access point, which is the first-run case and the usual recovery path.
  if (wifiEnabled && (WiFi.getMode() & WIFI_AP) != 0) startMdnsServices();

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
      // collectHeaders() narrows the set to the listed keys, which silently
      // dropped Accept-Encoding and disabled the gzip path. Ask for everything:
      // this core has no per-request cost worth worrying about here.
      server.collectAllHeaders();
      server.on("/", handleRootEntry);
      server.on("/app", handleRoot);
      server.on("/ui", HTTP_POST, handleUiSwitch);
      server.on("/lite", handleLite);
      server.on("/lite.html", handleLite);
server.on("/leaf.webp", handleLeafImg);
server.on("/leaf.png", handleLeafImg);
// status-picture overlays (only the ones present in the build are served)
for (size_t i = 0; i < WEBAPP_ASSET_COUNT; i++) {
  server.on(WEBAPP_ASSETS[i].path, HTTP_GET, handleWebAppImage);
}
      server.on("/api/status", handleApiStatus);
      server.on("/api/config", HTTP_GET, handleApiConfigGet);
      server.on("/api/config", HTTP_POST, handleApiConfigSave);
      server.on("/api/time", HTTP_POST, handleApiTime);
      server.on("/api/time", HTTP_GET, handleApiTime);
      server.on("/api/evscan", HTTP_POST, handleApiEvScan);
      server.on("/api/wifi_scan", HTTP_GET, handleWifiScan);
      server.on("/api/developer/login", HTTP_POST, handleDeveloperLogin);
      server.on("/api/developer/canlog", HTTP_GET, handleCanLogStatus);
      server.on("/api/developer/canwatch", HTTP_GET, handleCanWatch);
      server.on("/api/developer/canwatch/reset", HTTP_POST, handleCanWatchReset);
      server.on("/api/developer/replay", HTTP_POST, handleReplaySend);
      server.on("/api/developer/replay/arm", HTTP_POST, handleReplayArm);
      server.on("/api/developer/replay/disarm", HTTP_POST, handleReplayDisarm);
      server.on("/api/developer/vin_probe", HTTP_POST, handleVinProbe);
      server.on("/api/vin_probe", HTTP_POST, handleVinProbePublic);
      server.on("/api/modem_probe", HTTP_POST, handleModemProbe);
      server.on("/api/modem_power", HTTP_POST, handleModemPower);
      server.on("/api/trips", HTTP_GET, handleTripsList);
      server.on("/api/charges", HTTP_GET, handleChargesList);
      server.on("/api/trips/clear", HTTP_POST, handleTripsClear);
      server.on("/api/charges/clear", HTTP_POST, handleChargesClear);
      server.on("/api/tariff", HTTP_POST, handleTariffSet);
      server.on("/reboot", HTTP_POST, handleReboot);
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
          // Captive-portal detection probes get a bare 204. Redirecting them to
          // the UI used to make Android open its CaptivePortalLogin window,
          // which has a system swipe-to-refresh that page JS cannot disable
          // (and its pull-down reloaded the setup page). With a 204 Android
          // marks the network as "no portal" and the user opens the setup page
          // in a normal browser instead, where the PTR block works.
          const String& uri = server.uri();
          bool probe = uri == "/generate_204" || uri == "/gen_204" ||
                       uri == "/hotspot-detect.html" || uri == "/library/test/success.html" ||
                       uri == "/ncsi.txt" || uri == "/connecttest.txt" ||
                       uri == "/canonical.html" || uri == "/success.txt";
          if (probe) {
            server.send(204, "text/plain", "");
            return;
          }
          String host = server.hostHeader();
          bool foreignHost = host.length() > 0 &&
                             host != WiFi.softAPIP().toString() &&
                             host != "leaf.local";
          if (foreignHost) {
            // A browser typed a real domain: bring it to the UI (the page's own
            // PTR block applies there, unlike the system portal window)
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
    recordSoftwareRestart(rebootPendingReason);
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
  // Without an uplink the MQTT/TLS connect attempts just block the loop for
  // seconds each time, which makes the page crawl when the board is reached
  // through its own access point.
  if (activeSequence == CanSequence::NONE && !otaInProgress && WiFi.status() == WL_CONNECTED) {
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
    tripLogTick();
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
  managePendingPublishes();

  // --- Power save: deep sleep after idle period ---
  // Never sleep while charging or running remote climate
  bool callControlKeepsAwake = getModemEnable() && getModemCallEnabled();
  if (getSleepEnabled() && !callControlKeepsAwake && !canLoggerIsRecording() &&
      !otaInProgress && activeSequence == CanSequence::NONE &&
      !isChargingNow && !tripLogIsCharging() && !isHvacOn) {
    static unsigned long lastIdleTickMs = 0;
    unsigned long now = millis();
    if (lastIdleTickMs == 0) lastIdleTickMs = now;
    if (now - lastIdleTickMs >= 1000) {
      uint32_t secs = (now - lastIdleTickMs) / 1000;
      rtcState.idleSec += secs;
      lastIdleTickMs += secs * 1000UL;
    }

    // After timer wake, stay awake for a grace window (web UI / MQTT publish).
    // 180 s: enough for the first CAN refresh + reconnect + full metric sync
    // even on the modem link, where the reconnect alone can take ~10 s.
    const unsigned long WAKE_GRACE_MS = 180000;
    bool graceOver = !wokeFromTimer || (millis() >= WAKE_GRACE_MS);

    if (graceOver && rtcState.idleSec >= (uint64_t)getSleepAfterHours() * 3600ULL) {
      uint64_t intervalMin = (rtcState.idleSec >= 48ULL * 3600ULL)
                             ? (uint64_t)getWakeSlowMinutes()
                             : (uint64_t)getWakeFastMinutes();
      Serial.printf("### Deep sleep: idle %llu s, wake in %llu min ###\n",
                    (unsigned long long)rtcState.idleSec,
                    (unsigned long long)intervalMin);
      mqttGoingOffline();   // retained "connected = no" before power drop
      modemPowerOffNow();
      saveToRtc();
      esp_sleep_enable_timer_wakeup(intervalMin * 60ULL * 1000000ULL);
      delay(100);
      esp_deep_sleep_start();
    }
  }
}
