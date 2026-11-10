#include "serverApi.h"

#include <Preferences.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "canLeafZE1.h"
#include "tripLog.h"
#include <Update.h>
#include "configuration.h"
#include "serverCa.h"

// Р“Р»РѕР±Р°Р»СЊРЅС‹Рµ Р·РЅР°С‡РµРЅРёСЏ, РєРѕС‚РѕСЂС‹Рµ РІРµРґС‘С‚ РѕСЃРЅРѕРІРЅРѕР№ СЃРєРµС‚С‡.
extern float currentSOC;
extern float cabinTemp;
extern bool isHvacOn;
extern bool isChargingNow;
extern bool doorFL, doorFR, doorRL, doorRR, doorTrunk;
extern int8_t lockState;
extern EvBatteryData evData;
// Версия прошивки, объявлена в основном скетче.
extern const char* leafFwVersion;

// ---------------------------------------------------------------- РЅР°СЃС‚СЂРѕР№РєРё

// РђРґСЂРµСЃ СЃРµСЂРІРµСЂР° РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ. РњРѕР¶РЅРѕ РїРµСЂРµРѕРїСЂРµРґРµР»РёС‚СЊ РІ NVS (РєР»СЋС‡ "base").
#ifndef LEAF_SERVER_BASE
#define LEAF_SERVER_BASE "https://leaf.wargaelanor.keenetic.link"
#endif

static Preferences prefs;
static bool prefsReady = false;

static char cfgBase[96] = LEAF_SERVER_BASE;
static char cfgLogin[24] = {0};
static char cfgPass[20] = {0};
static char cfgToken[64] = {0};
static char claimCode[24] = {0};
static bool cfgEnabled = true;
static volatile bool configDirty = false;

static ServerCommandHandler cmdHandler = nullptr;

// ---------------------------------------------------------------- СЃРѕСЃС‚РѕСЏРЅРёРµ

static unsigned long lastStateMs = 0;
static unsigned long lastPollMs = 0;
static unsigned long lastRegisterMs = 0;
static unsigned long failBackoffMs = 0;
static uint32_t stateCounter = 0;
static bool registered = false;
static volatile unsigned long lastOkMs = 0;
static char statusText[48] = "off";

// Идентификаторы команд, которые надо подтвердить в следующем запросе.
static int pendingAck[8];
static uint8_t pendingAckCount = 0;

// В«РЎРІРµР¶РµСЃС‚СЊВ» РґР°РЅРЅС‹С…: РєРѕРіРґР° РІ РїРѕСЃР»РµРґРЅРёР№ СЂР°Р· РјРµРЅСЏР»РёСЃСЊ РїРѕРєР°Р·Р°РЅРёСЏ С€РёРЅС‹.
static unsigned long lastCanActivityMs = 0;
static float prevSocFingerprint = -1.0f;
static uint32_t prevOdoFingerprint = 0;

static uint32_t busAgeSeconds() {
  if (lastCanActivityMs == 0) return 0;
  return (millis() - lastCanActivityMs) / 1000UL;
}

static void noteCanActivity() {
  float soc = evData.instrumentSOC >= 0 ? evData.instrumentSOC : currentSOC;
  if (soc != prevSocFingerprint || evData.odometerKm != prevOdoFingerprint) {
    prevSocFingerprint = soc;
    prevOdoFingerprint = evData.odometerKm;
    lastCanActivityMs = millis();
  }
}

// РѕС‡РµСЂРµРґСЊ Р·Р°РїРёСЃРµР№ РїРѕРµР·РґРѕРє/Р·Р°СЂСЏРґРѕРє
static const int REC_QUEUE = 8;
static char recQueue[REC_QUEUE][220];
static char recKind[REC_QUEUE][8];
static int recHead = 0, recTail = 0, recCount = 0;

// ---------------------------------------------------------------- СѓС‚РёР»РёС‚С‹

static void rndHex(char* out, size_t len) {
  static const char* abc = "abcdefghijkmnpqrstuvwxyz23456789";
  for (size_t i = 0; i < len; i++) out[i] = abc[esp_random() % 30];
  out[len] = '\0';
}

static void loadConfig() {
  if (!prefs.begin("leafsrv", false)) {
    Serial.println("[srv] NVS РЅРµРґРѕСЃС‚СѓРїРЅРѕ, СЂР°Р±РѕС‚Р°РµРј РЅР° Р·РЅР°С‡РµРЅРёСЏС… РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ");
    return;
  }
  prefsReady = true;
  String base = prefs.getString("base", LEAF_SERVER_BASE);
  snprintf(cfgBase, sizeof(cfgBase), "%s", base.c_str());
  cfgEnabled = prefs.getBool("enabled", true);

  String login = prefs.getString("login", "");
  String pass = prefs.getString("pass", "");
  String token = prefs.getString("token", "");

  if (login.length() == 0 || pass.length() == 0) {
    // РџРµСЂРІС‹Р№ Р·Р°РїСѓСЃРє: РІС‹РґР°С‘Рј СЃРѕР±СЃС‚РІРµРЅРЅС‹Рµ СѓС‡С‘С‚РЅС‹Рµ РґР°РЅРЅС‹Рµ.
    char l[24], p[20];
    char suffix[8];
    rndHex(suffix, 6);
    snprintf(l, sizeof(l), "leaf-%s", suffix);
    rndHex(p, 12);
    login = l;
    pass = p;
    prefs.putString("login", login);
    prefs.putString("pass", pass);
    Serial.println();
    Serial.println("==================================================");
    Serial.println("[srv] РЈСЃС‚СЂРѕР№СЃС‚РІРѕ СЃРѕР·РґР°Р»Рѕ СЃРІРѕРё СѓС‡С‘С‚РЅС‹Рµ РґР°РЅРЅС‹Рµ:");
    Serial.printf("[srv]   Р»РѕРіРёРЅ:  %s\n", login.c_str());
    Serial.printf("[srv]   РїР°СЂРѕР»СЊ: %s\n", pass.c_str());
    Serial.println("[srv] Р’РІРµРґРёС‚Рµ РёС… РІ РІРµР±-РёРЅС‚РµСЂС„РµР№СЃРµ СЃРµСЂРІРµСЂР° РїРѕСЃР»Рµ");
    Serial.println("[srv] РѕРґРѕР±СЂРµРЅРёСЏ СѓСЃС‚СЂРѕР№СЃС‚РІР° Р°РґРјРёРЅРёСЃС‚СЂР°С‚РѕСЂРѕРј.");
    Serial.println("==================================================");
    Serial.println();
  }

  snprintf(cfgLogin, sizeof(cfgLogin), "%s", login.c_str());
  snprintf(cfgPass, sizeof(cfgPass), "%s", pass.c_str());
  snprintf(cfgToken, sizeof(cfgToken), "%s", token.c_str());
}

static void saveToken(const char* token) {
  snprintf(cfgToken, sizeof(cfgToken), "%s", token);
  if (prefsReady) prefs.putString("token", cfgToken);
}

static bool splitUrl(const char* url, String& host, uint16_t& port, String& path) {
  String u(url);
  bool tls = true;
  if (u.startsWith("https://")) {
    u = u.substring(8);
    port = 443;
    tls = true;
  } else if (u.startsWith("http://")) {
    u = u.substring(7);
    port = 80;
    tls = false;
  } else {
    port = 443;
  }
  int slash = u.indexOf('/');
  if (slash >= 0) {
    path = u.substring(slash);
    host = u.substring(0, slash);
  } else {
    host = u;
    path = "/";
  }
  return tls;
}

// Р’С‹РїРѕР»РЅСЏРµС‚ Р·Р°РїСЂРѕСЃ; РІРѕР·РІСЂР°С‰Р°РµС‚ HTTP-РєРѕРґ РёР»Рё РѕС‚СЂРёС†Р°С‚РµР»СЊРЅРѕРµ Р·РЅР°С‡РµРЅРёРµ РѕС€РёР±РєРё.

// Manual TLS request: HTTPClient returned an opaque -1, while a raw
// exchange shows the exact failure point.
// а ручной обмен показывает точную причину обрыва.
static int request(const char* method, const String& pathAndQuery, const char* body,
                   String& response, unsigned long timeoutMs = 8000) {
  String host, basePath;
  uint16_t port = 443;
  bool tls = splitUrl(cfgBase, host, port, basePath);

  WiFiClientSecure secure;
  WiFiClient plain;
  Client* cli = nullptr;
  if (tls) {
    secure.setCACert(SERVER_CA_PEM);
    secure.setTimeout(6000);
    secure.setHandshakeTimeout(8);
    cli = &secure;
  } else {
    plain.setTimeout(8000);
    cli = &plain;
  }

  unsigned long t0 = millis();
  bool connected = false;
  for (int attempt = 0; attempt < 1 && !connected; attempt++) {
    connected = (cli->connect(host.c_str(), port) == 1);
    if (!connected) {
      cli->stop();
      delay(400);
    }
  }
  if (!connected) {
    char err[80] = {0};
    if (tls) secure.lastError(err, sizeof(err));
    Serial.printf("[srv] connect fail %s:%u heap=%u tls=%d err=%s\n",
                  host.c_str(), port, (unsigned)ESP.getFreeHeap(), tls ? 1 : 0, err);
    return -2;
  }

  String path = pathAndQuery.startsWith("/") ? pathAndQuery : ("/" + pathAndQuery);
  String req = String(method) + " " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "User-Agent: LeafControl\r\n";
  req += "Accept: application/json\r\n";
  req += "Connection: close\r\n";
  req += "Content-Type: application/json\r\n";
  if (cfgToken[0]) req += String("Authorization: Bearer ") + cfgToken + "\r\n";
  if (body) {
    req += "Content-Length: " + String(strlen(body)) + "\r\n\r\n";
    req += body;
  } else {
    req += "Content-Length: 0\r\n\r\n";
  }
  cli->print(req);
  cli->flush();

  response = "";
  unsigned long deadline = millis() + 6000;
  while (millis() < deadline) {
    while (cli->available()) {
      response += (char)cli->read();
      if (response.length() > 8192) break;
    }
    if (!cli->connected() && !cli->available()) break;
    delay(1);
  }
  cli->stop();

  int code = -3;
  int sp = response.indexOf(' ');
  if (sp > 0) code = response.substring(sp + 1, sp + 4).toInt();
  Serial.printf("[srv] %s %s -> %d (%lu ms, heap=%u)\n", method, path.c_str(), code,
                (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap());
  return code;
}

static void setStatus(const char* s);  // forward

static bool postJson(const String& path, const char* body, String& resp) {
  int code = request("POST", path, body, resp);
  if (code >= 200 && code < 300) return true;
  // Токен отвергнут сервером — забываем его и регистрируемся заново.
  if (code == 401 || code == 403) {
    saveToken("");
    registered = false;
    setStatus("re-enroll");
  }
  if (code > 0) Serial.printf("[srv] POST %s -> %d: %s\n", path.c_str(), code, resp.substring(0, 160).c_str());
  else Serial.printf("[srv] POST %s -> transport error %d\n", path.c_str(), code);
  return false;
}

static void setStatus(const char* s) {
  snprintf(statusText, sizeof(statusText), "%s", s);
}

// Диагностика канала: DNS и TCP до сервера. Печатается, когда регистрация
// не проходит, чтобы по serial было видно, где именно обрыв.
static void netDiag() {
  IPAddress ip;
  int ok = WiFi.hostByName("leaf.wargaelanor.keenetic.link", ip);
  Serial.printf("[srv] net ip=%s gw=%s dns=%s resolve=%d(%s)\n",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.dnsIP().toString().c_str(),
                ok, ip.toString().c_str());
  if (ok == 1) {
    WiFiClient probe;
    bool conn = probe.connect(ip, 443, 6000);
    Serial.printf("[srv] tcp443=%d\n", conn ? 1 : 0);
    if (conn) probe.stop();
  }
}

// ---------------------------------------------------------------- СЂРµРіРёСЃС‚СЂР°С†РёСЏ

// Достаёт строковое значение по ключу, не завися от пробелов:
// {"device_token": "abc"} и {"device_token":"abc"} дают одинаковый результат.
static bool jsonString(const String& src, const char* key, char* out, size_t outSize) {
  out[0] = '\0';
  int k = src.indexOf(key);
  if (k < 0) return false;
  int colon = src.indexOf(':', k + strlen(key));
  if (colon < 0) return false;
  int q1 = src.indexOf('"', colon + 1);
  if (q1 < 0) return false;
  int q2 = src.indexOf('"', q1 + 1);
  if (q2 < 0) return false;
  String value = src.substring(q1 + 1, q2);
  snprintf(out, outSize, "%s", value.c_str());
  return out[0] != '\0';
}

static void sendConfig();  // forward declaration

static void registerDevice() {
  unsigned long now = millis();
  if (now - lastRegisterMs < 15000UL) return;
  lastRegisterMs = now;

  char body[320];
  snprintf(body, sizeof(body),
           "{\"login\":\"%s\",\"password\":\"%s\",\"name\":\"Leaf\",\"serial\":\"%s\",\"fw\":\"%s\"}",
           cfgLogin, cfgPass, WiFi.macAddress().c_str(), leafFwVersion);

  String resp;
  if (!postJson("/api/device/enroll", body, resp)) {
    setStatus("enroll failed");
    return;
  }
  char token[64];
  if (!jsonString(resp, "device_token", token, sizeof(token))) {
    setStatus("enroll: no token");
    Serial.printf("[srv] enroll body: %s\n", resp.substring(0, 200).c_str());
    return;
  }
  saveToken(token);
  registered = true;
  char status[16];
  jsonString(resp, "status", status, sizeof(status));
  char code[24] = {0};
  if (jsonString(resp, "claim_code", code, sizeof(code))) {
    snprintf(claimCode, sizeof(claimCode), "%s", code);
  }
  if (!strcmp(status, "active")) claimCode[0] = '\0';
  bool active = strcmp(status, "active") == 0;
  setStatus(active ? "active" : "pending approval");
  Serial.printf("[srv] registered, status: %s\n", active ? "active" : "pending");
  sendConfig();
}

// ---------------------------------------------------------------- СЃРѕСЃС‚РѕСЏРЅРёРµ

static void sendHeartbeat() {
  char body[160];
  snprintf(body, sizeof(body),
           "{\"link\":\"wifi\",\"rssi\":%d,\"bus_age_s\":%lu}",
           WiFi.RSSI(), (unsigned long)busAgeSeconds());
  String resp;
  postJson("/api/device/heartbeat", body, resp);
}

static String stateJson() {
  float soc = evData.instrumentSOC >= 0 ? evData.instrumentSOC : currentSOC;
  float packV = evData.packVoltage;
  float packI = evData.packCurrent;
  float packKw = (packV > 0 && evData.packDataValid) ? (packV * packI) / 1000.0f : 0.0f;
#if defined(LEAF_SYNTH_TELEMETRY) && (LEAF_SYNTH_TELEMETRY)
  // Стендовый режим: CAN нет, поэтому показываем правдоподобные значения,
  // чтобы проверить обмен, графики и историю. В машине флаг выключен.
  {
    static const uint32_t t0 = millis();
    float ph = (millis() - t0) / 1000.0f;
    if (soc < 0) soc = 64.0f + 8.0f * sinf(ph / 600.0f);
    if (evData.odometerKm == 0) evData.odometerKm = 51234 + (uint32_t)((millis() - t0) / 60000);
    if (cabinTemp < -50) cabinTemp = 12.0f + 3.0f * sinf(ph / 300.0f);
    if (evData.ambientTemp < -50) evData.ambientTemp = -2.5f;
    if (evData.batteryTemp < -50) evData.batteryTemp = 11.0f + 2.0f * sinf(ph / 900.0f);
    if (packV <= 0) packV = 383.4f;
    if (packKw == 0) packKw = 0.0f;
  }
#endif
  time_t tnow = time(nullptr);
  uint32_t ts = (tnow > 1600000000) ? (uint32_t)tnow : 0;

  char body[420];
  snprintf(body, sizeof(body),
           "{\"ts\":%lu,\"soc\":%.1f,\"odo\":%.0f,\"cabin\":%.1f,\"amb\":%.1f,"
           "\"batt\":%.1f,\"pack_v\":%.1f,\"pack_kw\":%.2f,\"v12\":%.1f,"
           "\"hvac\":%d,\"charging\":%d,\"locked\":%d,\"doors\":%d,\"soh\":%d,\"spd\":%.1f,\"bus_age_s\":%lu}",
           (unsigned long)ts,
           soc >= 0 ? soc : -1.0,
           (double)evData.odometerKm,
           cabinTemp, evData.ambientTemp,
           evData.batteryTemp, packV, packKw, 0.0,
           isHvacOn ? 1 : 0, (isChargingNow || tripLogIsCharging()) ? 1 : 0,
           (lockState == 1) ? 1 : 0,
           (doorFL || doorFR || doorRL || doorRR || doorTrunk) ? 1 : 0,
           evData.soh, 0.0,
           (unsigned long)busAgeSeconds());

  return String(body);
}

static bool sendState() {
  String payload = stateJson();
  String resp;
  bool ok = postJson("/api/device/state", payload.c_str(), resp);
  if (ok) {
    stateCounter++;
    setStatus(registered ? "online" : "online (pending)");
  }
  return ok;
}

// ---------------------------------------------------------------- РєРѕРјР°РЅРґС‹

// ---------------------------------------------------------------- settings

static void sendConfig() {
  char body[420];
  snprintf(body, sizeof(body),
           "{\"remote_mode\":%d,\"charge_limit\":%d,\"locking\":%d,\"sleep\":%d,\"sleep_after\":%d,"
           "\"wake_fast\":%d,\"wake_slow\":%d,\"uds\":%d,\"uds_tpms\":%d,"
           "\"modem\":%d,\"apn\":\"%s\"}",
           (int)getRemoteTransport(),
           getChargeLimitPct(), getLockingEnabled() ? 1 : 0, getSleepEnabled() ? 1 : 0,
           getSleepAfterHours(), getWakeFastMinutes(), getWakeSlowMinutes(),
           getUdsEnabled() ? 1 : 0, getUdsTpmsEnabled() ? 1 : 0,
           getModemEnable() ? 1 : 0, getModem800Apn());
  String resp;
  postJson("/api/device/config", body, resp);
}

static void applyConfig(const char* argsJson) {
  char key[24] = {0};
  if (!jsonString(argsJson, "\"key\"", key, sizeof(key))) return;

  const char* vp = strstr(argsJson, "\"value\"");
  if (!vp) return;
  vp = strchr(vp, ':');
  if (!vp) return;
  vp++;
  while (*vp == ' ') vp++;
  int ival = atoi(vp);
  bool bval = (ival != 0) || (strncmp(vp, "true", 4) == 0);

  if (!strcmp(key, "remote_mode")) {
    RemoteTransport mode = bval ? RemoteTransport::SERVER : RemoteTransport::LOCAL;
    setRemoteTransport(mode);
    setUiLite(mode == RemoteTransport::SERVER);
  }
  else if (!strcmp(key, "charge_limit")) setChargeLimitPct(ival);
  else if (!strcmp(key, "locking")) setLockingEnabled(bval);
  else if (!strcmp(key, "sleep")) setSleepEnabled(bval);
  else if (!strcmp(key, "sleep_after")) setSleepAfterHours(ival);
  else if (!strcmp(key, "wake_fast")) setWakeFastMinutes(ival);
  else if (!strcmp(key, "wake_slow")) setWakeSlowMinutes(ival);
  else if (!strcmp(key, "uds")) setUdsEnabled(bval);
  else if (!strcmp(key, "uds_tpms")) setUdsTpmsEnabled(bval);
  else if (!strcmp(key, "modem")) setModemEnable(bval);
  else {
    Serial.printf("[srv] unknown config key: %s\n", key);
    return;
  }
  Serial.printf("[srv] config %s = %d\n", key, ival);
  configDirty = true;
}

// ---------------------------------------------------------------- мост команд
// Сетевой поток не трогает CAN и не запускает последовательности: он только
// кладёт команду в почтовый ящик. Основной цикл забирает её, выполняет и
// возвращает результат — только после этого команда подтверждается серверу.

struct CmdBox {
  int id;
  char name[28];
  char args[192];
};

static portMUX_TYPE mboxMux = portMUX_INITIALIZER_UNLOCKED;
static CmdBox inbox;
static volatile bool inboxReady = false;
static volatile bool inflight = false;
static int inflightId = 0;
static char inflightName[28] = {0};
static unsigned long inflightAt = 0;
static int outAck[6];
static uint8_t outAckCount = 0;
static int outNack[6];
static uint8_t outNackCount = 0;

// Команды, которые обязаны запустить CAN-последовательность. Если после вызова
// обработчика последовательность не началась, команда считается неудачной.
static bool commandNeedsSequence(const char* name) {
  static const char* list[] = {"clim_on", "clim_off", "lock", "unlock",
                               "lights", "horn", "refresh",
                               "charge_start", "charge_stop"};
  for (const char* n : list) {
    if (!strcmp(name, n)) return true;
  }
  return false;
}

static void mailboxComplete(bool ok) {
  portENTER_CRITICAL(&mboxMux);
  if (ok) {
    if (outAckCount < 6) outAck[outAckCount++] = inflightId;
  } else {
    if (outNackCount < 6) outNack[outNackCount++] = inflightId;
  }
  portEXIT_CRITICAL(&mboxMux);
  Serial.printf("[srv] cmd %d -> %s\n", inflightId, ok ? "done" : "failed");
  inflight = false;
  inflightId = 0;
}

// Вызывается из loop(): выполняет команду из ящика и закрывает текущую,
// когда последовательность закончилась.
void serverApiCommandPump() {
  if (inflight) {
    bool done = (activeSequence == CanSequence::NONE);
    bool timeout = (millis() - inflightAt) > 30000UL;
    if (!done && !timeout) return;
    bool ok = done && !lastSequenceFailed;
    // Замок и двери проверяем по факту: машина должна сообщить новое состояние.
    if (ok && !strcmp(inflightName, "lock")) ok = (lockState == 1);
    if (ok && !strcmp(inflightName, "unlock")) ok = (lockState == 0);
    mailboxComplete(ok);
    return;
  }
  if (!inboxReady) return;
  if (activeSequence != CanSequence::NONE) return;

  CmdBox cmd;
  portENTER_CRITICAL(&mboxMux);
  cmd = inbox;
  inboxReady = false;
  portEXIT_CRITICAL(&mboxMux);

  inflight = true;
  inflightId = cmd.id;
  snprintf(inflightName, sizeof(inflightName), "%s", cmd.name);
  inflightAt = millis();
  Serial.printf("[srv] cmd %d: %s %s\n", cmd.id, cmd.name, cmd.args);

  if (!strcmp(cmd.name, "set_config")) {
    applyConfig(cmd.args);
    mailboxComplete(true);
    return;
  }
  if (cmdHandler) cmdHandler(cmd.name, cmd.args);
  if (!commandNeedsSequence(cmd.name)) {
    mailboxComplete(true);           // настройки, синхронизация конфига и т. п.
  } else if (activeSequence == CanSequence::NONE) {
    mailboxComplete(false);          // последовательность не запустилась
  }
}

// ---------------------------------------------------------------- прошивка

// Скачивает .bin с сервера и прошивает плату, затем перезагружается.
// Проверяет код ответа и длину: битый или неполный файл в flash не пишется.
static bool flashFromUrl(const char* url) {
  String host, path;
  uint16_t port = 443;
  bool tls = splitUrl(url, host, port, path);
  WiFiClientSecure secure;
  WiFiClient plain;
  Client* cli = tls ? (Client*)&secure : (Client*)&plain;
  if (tls) secure.setCACert(SERVER_CA_PEM);
  if (!cli->connect(host.c_str(), port)) {
    Serial.println("[srv] ota: connect failed");
    return false;
  }
  cli->setTimeout(15000);
  String req = String("GET ") + path + " HTTP/1.1\r\nHost: " + host +
               "\r\nConnection: close\r\nUser-Agent: LeafControl\r\n\r\n";
  cli->print(req);

  // ---- заголовки
  String status = cli->readStringUntil('\n');
  int code = status.length() > 12 ? status.substring(9, 12).toInt() : 0;
  long contentLen = -1;
  while (cli->connected()) {
    String line = cli->readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
    if (line.startsWith("Content-Length:")) contentLen = line.substring(15).toInt();
  }
  if (code != 200) {
    Serial.printf("[srv] ota: http %d\n", code);
    cli->stop();
    return false;
  }
  if (contentLen > 0) {
    if (!Update.begin((size_t)contentLen)) {
      Serial.printf("[srv] ota: begin failed (need %ld)\n", contentLen);
      cli->stop();
      return false;
    }
  } else if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    Serial.println("[srv] ota: begin failed");
    cli->stop();
    return false;
  }

  uint8_t buf[1024];
  size_t total = 0;
  unsigned long lastData = millis();
  while (cli->connected() || cli->available()) {
    size_t n = cli->readBytes(buf, sizeof(buf));
    if (n) {
      if (Update.write(buf, n) != n) {
        Serial.println("[srv] ota: write failed");
        Update.abort();
        cli->stop();
        return false;
      }
      total += n;
      lastData = millis();
    } else {
      delay(5);
      if (millis() - lastData > 30000UL) break;
    }
  }
  cli->stop();

  if (contentLen > 0 && total != (size_t)contentLen) {
    Serial.printf("[srv] ota: short read %u of %ld\n", (unsigned)total, contentLen);
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    Serial.printf("[srv] ota: end failed (%u bytes)\n", (unsigned)total);
    return false;
  }
  Serial.printf("[srv] ota: flashed %u bytes, rebooting\n", (unsigned)total);
  delay(800);
  ESP.restart();
  return true;
}

// Лёгкий признак жизни: сервер видит, что плата на связи и какая у неё
// прошивка, даже если полная синхронизация состояния не проходит.
static bool sendPing() {
  char body[220];
  snprintf(body, sizeof(body),
           "{\"fw\":\"%s\",\"link\":\"%s\",\"rssi\":%d,\"uptime\":%lu}",
           leafFwVersion,
           WiFi.status() == WL_CONNECTED ? "wifi" : "none",
           WiFi.RSSI(),
           (unsigned long)(millis() / 1000UL));
  String resp;
  return postJson("/api/device/ping", body, resp);
}

static bool syncRequest() {
  // Тело = обычное состояние плюс связка, прошивка и подтверждения команд.
  String payload = stateJson();
  payload.remove(payload.length() - 1);  // убираем закрывающую скобку

  payload += ",\"plug\":";
  payload += (tripLogIsCharging() || isChargingNow) ? "1" : "0";
  payload += ",\"link\":\"wifi\",\"rssi\":";
  payload += String(WiFi.RSSI());
  payload += ",\"fw\":\"";
  payload += leafFwVersion;
  payload += "\"";

  payload += ",\"ack\":[";
  portENTER_CRITICAL(&mboxMux);
  for (uint8_t i = 0; i < outAckCount; i++) {
    if (i) payload += ",";
    payload += String(outAck[i]);
  }
  portEXIT_CRITICAL(&mboxMux);
  payload += "],\"nack\":[";
  portENTER_CRITICAL(&mboxMux);
  for (uint8_t i = 0; i < outNackCount; i++) {
    if (i) payload += ",";
    payload += String(outNack[i]);
  }
  portEXIT_CRITICAL(&mboxMux);
  payload += "]}";

  String resp;
  if (!postJson("/api/device/sync", payload.c_str(), resp)) return false;

  portENTER_CRITICAL(&mboxMux);
  outAckCount = 0;
  outNackCount = 0;
  portEXIT_CRITICAL(&mboxMux);

  // Разбор очереди: {"ok":true,"cmd":{"id":7,"name":"lock","args":{}},...}
  if (resp.indexOf("\"cmd\":null") < 0 && resp.indexOf("\"cmd\": null") < 0) {
    int idPos = resp.indexOf("\"id\":");
    long id = idPos >= 0 ? resp.substring(idPos + 5).toInt() : 0;
    char name[28];
    if (id > 0 && jsonString(resp, "\"name\"", name, sizeof(name))) {
      CmdBox cmd = {};
      cmd.id = (int)id;
      snprintf(cmd.name, sizeof(cmd.name), "%s", name);
      snprintf(cmd.args, sizeof(cmd.args), "{}");

      // Аргументы — ровно объект после "args", без хвоста ответа.
      int argsKey = resp.indexOf("\"args\"");
      if (argsKey >= 0) {
        int brace = resp.indexOf('{', argsKey);
        if (brace >= 0) {
          int depth = 0;
          for (int i = brace; i < (int)resp.length(); i++) {
            if (resp[i] == '{') depth++;
            else if (resp[i] == '}') {
              depth--;
              if (depth == 0) {
                snprintf(cmd.args, sizeof(cmd.args), "%s",
                         resp.substring(brace, i + 1).c_str());
                break;
              }
            }
          }
        }
      }
      // Прошивка живёт в сетевом потоке: она качает файл и перезагружает плату.
      if (!strcmp(cmd.name, "fw_update")) {
        char url[160];
        if (jsonString(cmd.args, "\"url\"", url, sizeof(url))) {
          Serial.printf("[srv] ota: %s\n", url);
          flashFromUrl(url);
        }
        portENTER_CRITICAL(&mboxMux);
        if (outNackCount < 6) outNack[outNackCount++] = cmd.id;
        portEXIT_CRITICAL(&mboxMux);
        return true;
      }

      portENTER_CRITICAL(&mboxMux);
      inbox = cmd;
      inboxReady = true;
      portEXIT_CRITICAL(&mboxMux);
    }
  }

  if (resp.indexOf("\"config\":true") >= 0 || resp.indexOf("\"config\": true") >= 0) {
    configDirty = true;
  }

  // Сервер узнал о нас: если плата была pending, проверяем статус.
  if (resp.indexOf("\"online\":false") >= 0) {
    registerDevice();
  }
  setStatus(registered ? "online" : "online (pending)");
  return true;
}

static void handleCommandResponse(const String& resp) {
  // {"id": 7, "name": "clim_on", "args": {"minutes": 20}} либо {"id": null}
  if (resp.indexOf("\"id\": null") >= 0 || resp.indexOf("\"id\":null") >= 0) return;

  int idPos = resp.indexOf("\"id\":");
  if (idPos < 0) return;
  long id = resp.substring(idPos + 5).toInt();
  if (id <= 0) return;

  char name[28];
  if (!jsonString(resp, "\"name\"", name, sizeof(name))) return;

  // args берём как остаток после "args" — формат простой, вложенности нет
  String args = "{}";
  int aPos = resp.indexOf("\"args\"");
  if (aPos >= 0) {
    int brace = resp.indexOf('{', aPos);
    if (brace >= 0) args = resp.substring(brace, resp.lastIndexOf('}'));
  }

  Serial.printf("[srv] command: %s %s\n", name, args.c_str());
  if (!strcmp(name, "set_config")) applyConfig(args.c_str());
  else if (!strcmp(name, "sync_config")) sendConfig();
  else if (cmdHandler) cmdHandler(name, args.c_str());

  char ack[220];
  snprintf(ack, sizeof(ack), "{\"id\":%ld,\"ok\":true,\"result\":{\"cmd\":\"%s\"}}", id, name);
  String r;
  postJson("/api/device/ack", ack, r);
}

static bool pollCommands() {
  String resp;
  int code = request("GET", "/api/device/cmd?wait=0", nullptr, resp, 6000);
  if (code >= 200 && code < 300) {
    handleCommandResponse(resp);
    return true;
  }
  Serial.printf("[srv] poll -> %d %s\n", code, resp.substring(0, 120).c_str());
  return false;
}

// ---------------------------------------------------------------- Р·Р°РїРёСЃРё

void serverApiOnRecord(const char* kind, const char* line) {
  if (!cfgEnabled || recCount >= REC_QUEUE) return;
  snprintf(recQueue[recTail], sizeof(recQueue[recTail]), "%s", line);
  snprintf(recKind[recTail], sizeof(recKind[recTail]), "%s", kind);
  recTail = (recTail + 1) % REC_QUEUE;
  recCount++;
}

// РџР»Р°С‚Р° С…СЂР°РЅРёС‚ СЃРІРѕРё Р·Р°РїРёСЃРё РєР°Рє {"s":start,"e":end,"d":СЃРµРє,"k":РєРј,"w":Wh,"s0":..,"s1":..}
// РЎРµСЂРІРµСЂ Р¶РґС‘С‚ {kind,started_at,ended_at,duration_s,distance_km,energy_kwh,soc_start,soc_end}
static bool sendOneRecord() {
  if (recCount == 0) return false;
  const char* line = recQueue[recHead];
  const char* kind = recKind[recHead];
  recHead = (recHead + 1) % REC_QUEUE;
  recCount--;

  auto field = [&](const char* key, double def) -> double {
    int p = strstr(line, key) ? (int)(strstr(line, key) - line) : -1;
    if (p < 0) return def;
    return atof(line + p + strlen(key));
  };
  double s = field("\"s\":", 0);
  double e = field("\"e\":", 0);
  double d = field("\"d\":", 0);
  double km = field("\"k\":", 0);
  double wh = field("\"w\":", 0);
  double s0 = field("\"s0\":", -1);
  double s1 = field("\"s1\":", -1);

  char body[320];
  snprintf(body, sizeof(body),
           "{\"kind\":\"%s\",\"started_at\":%lu,\"ended_at\":%lu,\"duration_s\":%lu,"
           "\"distance_km\":%.2f,\"energy_kwh\":%.3f,\"soc_start\":%.0f,\"soc_end\":%.0f}",
           strcmp(kind, "charge") == 0 ? "charge" : "trip",
           (unsigned long)s, (unsigned long)e, (unsigned long)d,
           km, wh / 1000.0, s0, s1);

  String resp;
  if (postJson("/api/device/session", body, resp)) {
    Serial.printf("[srv] Р·Р°РїРёСЃСЊ %s РѕС‚РїСЂР°РІР»РµРЅР°\n", kind);
    return true;
  }
  // РІРµСЂРЅСѓС‚СЊ РІ РѕС‡РµСЂРµРґСЊ РїСЂРё РЅРµСѓРґР°С‡Рµ
  recHead = (recHead + REC_QUEUE - 1) % REC_QUEUE;
  recCount++;
  return false;
}

// ---------------------------------------------------------------- API

void serverApiBegin() {
  loadConfig();
  Serial.printf("[srv] СЃРµСЂРІРµСЂ: %s, СѓС‡С‘С‚РєР°: %s, %s\n", cfgBase, cfgLogin,
                cfgEnabled ? "РІРєР»СЋС‡РµРЅРѕ" : "РІС‹РєР»СЋС‡РµРЅРѕ");
}

void serverApiSetCommandHandler(ServerCommandHandler handler) { cmdHandler = handler; }
const char* serverApiLogin() { return cfgLogin; }
const char* serverApiPassword() { return cfgPass; }
const char* serverApiStatusText() { return statusText; }
const char* serverApiBase() { return cfgBase; }
const char* serverApiClaimCode() { return claimCode; }

bool serverApiReady() {
  return cfgEnabled && WiFi.status() == WL_CONNECTED && cfgLogin[0] != '\0';
}

// ---------------------------------------------------------------- сетевой поток
// Вся работа с TLS живёт в отдельной задаче на ядре 0. Основной цикл (веб-сервер,
// CAN, кнопки) при этом не останавливается: даже 30-секундное рукопожатие на
// слабом Wi-Fi больше не подвешивает плату.

static void serverNetOnce() {
  if (!cfgEnabled) return;
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("no wifi");
    return;
  }
  if (getRemoteTransport() != RemoteTransport::SERVER) return;
  unsigned long now = millis();

  static uint8_t failCount = 0;
  static unsigned long retryAt = 0;
  if (failCount >= 3 && (long)(now - retryAt) < 0) return;

  static bool registeredOnce = false;
  if (!registered || cfgToken[0] == '\0') {
    static unsigned long lastDbgMs = 0;
    if (now - lastDbgMs >= 20000UL) {
      lastDbgMs = now;
      Serial.printf("[srv] waiting registration: reg=%d token=%d wifi=%d\n",
                    registered ? 1 : 0, cfgToken[0] ? 1 : 0, (int)WiFi.status());
    }
    registerDevice();
    return;
  }

  static unsigned long lastSyncMs = 0;
  unsigned long interval = (WiFi.RSSI() < -80) ? 15000UL : 8000UL;
  static bool linkOk = false;
  bool syncOk = false;
  if (now - lastSyncMs >= interval) {
    lastSyncMs = now;
    syncOk = syncRequest();
    linkOk = syncOk;
    if (syncOk) {
      lastOkMs = millis();
      failCount = 0;
      if (recCount > 0) sendOneRecord();
    } else if (failCount < 255 && ++failCount >= 3) {
      retryAt = now + 30000UL;
    }
  }

  // Раз в минуту (и сразу после неудачной синхронизации) отправляем короткий
  // ping: сервер обновляет last_seen и версию прошивки.
  static unsigned long lastPingMs = 0;
  bool pingDue = (lastPingMs == 0) || (now - lastPingMs >= 60000UL) ||
                 (!linkOk && (now - lastPingMs >= 20000UL));
  if (pingDue && WiFi.status() == WL_CONNECTED) {
    if (sendPing()) {
      lastPingMs = now;
      lastOkMs = millis();
    }
  }

  if (configDirty) {
    configDirty = false;
    sendConfig();
  }

  static unsigned long lastCfgMs = 0;
  if (now - lastCfgMs >= 600000UL) {
    lastCfgMs = now;
    sendConfig();
  }
}

static void serverNetLoop(void* arg) {
  (void)arg;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      serverNetOnce();
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

unsigned long serverApiLastOkMs() { return lastOkMs; }

void serverApiTick() {
  static bool started = false;
  if (started) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (xTaskCreatePinnedToCore(serverNetLoop, "leaf-net", 16384, nullptr, 1, nullptr, 0) == pdPASS) {
    started = true;
    Serial.println("[srv] сетевой поток запущен");
  } else {
    Serial.println("[srv] ERROR: не удалось запустить сетевой поток");
  }
}



