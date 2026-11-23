#include "serverApi.h"

#include <Preferences.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_mac.h>
#include <WiFiUdp.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>
#include <lwip/dns.h>

#include "canLeafZE1.h"
#include "evCanLeaf.h"
#include "tripLog.h"
#include "canLogger.h"
#include "vehicleProfile.h"
#include "modemManager.h"
#include "modemHw.h"
#include <Update.h>
#include <esp_ota_ops.h>
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
// Рабочий A-адрес сервера: у имени два адреса, и один из них отдаёт 410 Gone
// (веб-морда Keenetic). Храним в NVS, чтобы после перезагрузки сразу идти
// проверенным путём и не тратить дефицитную память на перебор.
static IPAddress goodIp;
static bool haveGoodIp = false;
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

static uint32_t busAgeSeconds() {
  // Existing clients interpret this field as fage (EV-CAN freshness).
  // A CAR-CAN diagnostic reply cannot make battery telemetry fresh.
  return evCanLastFrameAgeSec();
}

// РѕС‡РµСЂРµРґСЊ Р·Р°РїРёСЃРµР№ РїРѕРµР·РґРѕРє/Р·Р°СЂСЏРґРѕРє
static const int REC_QUEUE = 24;   // 24*220 байт статики: heap и так в обрез
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

  String srvIp = prefs.getString("srvip", "");
  if (srvIp.length() > 0) {
    IPAddress ip;
    if (ip.fromString(srvIp)) {
      goodIp = ip;
      haveGoodIp = true;
      Serial.printf("[srv] рабочий адрес сервера: %s\n", srvIp.c_str());
    }
  }
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
  int colon = u.indexOf(':');
  if (colon >= 0) {
    // явный порт в URL: https://host:port или http://host:8090
    long p = u.substring(colon + 1).toInt();
    if (p > 0 && p < 65536) port = (uint16_t)p;
    u = u.substring(0, colon);
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
static int tlsVerifyFails = 0;          // подряд идущих X509-неудач
static const int TLS_FAIL_LIMIT = 3;    // после 3 подряд — fallback на setInsecure

// Имя leaf.wargaelanor.keenetic.link резолвится в два адреса (Keenetic DyDNS),
// и один из них отдаёт сертификат на *.keenetic.link: проверка имени там падает
// всегда, и плата оставалась без связи до перезагрузки. Поэтому после серии
// X509-неудач переходим на работу без проверки цепочки и держим этот режим,
// пока связь не восстановится проверенным соединением.
static bool tlsVerifyEnabled() { return tlsVerifyFails < TLS_FAIL_LIMIT; }

static void tlsConfigure(WiFiClientSecure& secure) {
  if (tlsVerifyEnabled()) secure.setCACert(SERVER_CA_PEM);
  else secure.setInsecure();  // fallback: сертификат сервера не сходится с именем
}

static void tlsNoteFailure(WiFiClientSecure& secure) {
  char err[80] = {0};
  secure.lastError(err, sizeof(err));
  // Считаем любые подряд идущие неудачи, а не только X509: при нехватке heap
  // mbedTLS отдаёт "Generic error", но лечится это тем же отключением проверки
  // (парсинг цепочки из трёх CA — самая дорогая часть рукопожатия).
  if (tlsVerifyFails < 100) tlsVerifyFails++;
  Serial.printf("[srv] tls: соединение не установлено (%d/%d): %s\n",
                tlsVerifyFails, TLS_FAIL_LIMIT, err);
}

static void tlsNoteSuccess(bool usedVerification) {
  // Успех без проверки ничего не доказывает: остаёмся в fallback-режиме.
  if (usedVerification) tlsVerifyFails = 0;
}

// lwIP отдаёт по имени только один A-адрес, а у Keenetic DyDNS их два: релей
// (отвечает API) и домашний роутер (на :443 отдаёт 410 Gone). Поэтому спрашиваем
// DNS сами и получаем полный список.
static int dnsQueryA(const String& host, IPAddress out[], int maxN) {
  IPAddress dnsIp = WiFi.dnsIP(0);
  if ((uint32_t)dnsIp == 0) return 0;

  uint8_t q[256];
  size_t p = 0;
  q[p++] = 0x4C; q[p++] = 0x46;          // ID
  q[p++] = 0x01; q[p++] = 0x00;          // флаги: обычный запрос
  q[p++] = 0x00; q[p++] = 0x01;          // QDCOUNT
  for (int i = 0; i < 6; i++) q[p++] = 0x00;  // AN/NS/AR counts
  const char* s = host.c_str();
  while (*s) {
    const char* dot = strchr(s, '.');
    size_t len = dot ? (size_t)(dot - s) : strlen(s);
    if (len == 0 || len > 63 || p + len + 2 >= sizeof(q)) return 0;
    q[p++] = (uint8_t)len;
    memcpy(q + p, s, len);
    p += len;
    if (!dot) break;
    s = dot + 1;
  }
  q[p++] = 0x00;                         // конец имени
  q[p++] = 0x00; q[p++] = 0x01;          // QTYPE = A
  q[p++] = 0x00; q[p++] = 0x01;          // QCLASS = IN

  WiFiUDP udp;
  if (!udp.begin(0)) return 0;
  udp.beginPacket(dnsIp, 53);
  udp.write(q, p);
  udp.endPacket();

  int n = 0;
  unsigned long deadline = millis() + 2500;
  while (millis() < deadline && n == 0) {
    int sz = udp.parsePacket();
    if (sz < 12) { delay(10); continue; }
    uint8_t r[512];
    int rl = udp.read(r, sizeof(r));
    if (rl < 12) continue;
    int qd = (r[4] << 8) | r[5];
    int an = (r[6] << 8) | r[7];
    int i = 12;
    for (int k = 0; k < qd && i < rl; k++) {     // пропускаем вопросы
      while (i < rl && r[i] != 0) {
        if ((r[i] & 0xC0) == 0xC0) { i++; break; }
        i += r[i] + 1;
      }
      i += 5;                                    // 0x00 + QTYPE + QCLASS
    }
    for (int k = 0; k < an && i + 10 < rl; k++) {
      if ((r[i] & 0xC0) == 0xC0) i += 2;         // сжатое имя
      else { while (i < rl && r[i] != 0) i += r[i] + 1; i++; }
      if (i + 10 > rl) break;
      uint16_t type = (r[i] << 8) | r[i + 1];
      uint16_t rdlen = (r[i + 8] << 8) | r[i + 9];
      i += 10;
      if (type == 1 && rdlen == 4 && i + 4 <= rl && n < maxN) {
        IPAddress ip(r[i], r[i + 1], r[i + 2], r[i + 3]);
        if ((uint32_t)ip != 0) out[n++] = ip;
      }
      i += rdlen;
    }
  }
  udp.stop();
  return n;
}

static int resolveAll(const String& host, IPAddress out[], int maxN) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), "443", &hints, &res) != 0 || !res) return 0;
  int n = 0;
  for (struct addrinfo* p = res; p && n < maxN; p = p->ai_next) {
    const struct sockaddr_in* sa = (const struct sockaddr_in*)p->ai_addr;
    if (!sa) continue;
    IPAddress ip((uint32_t)sa->sin_addr.s_addr);
    bool dup = false;
    for (int i = 0; i < n; i++) if (out[i] == ip) dup = true;
    if (!dup) out[n++] = ip;
  }
  freeaddrinfo(res);
  return n;
}

static int plainByIp(const String& host, const IPAddress& ip, const String& req,
                     String& response);

static int request(const char* method, const String& pathAndQuery, const char* body,
                   String& response, unsigned long timeoutMs = 8000) {
  String host, basePath;
  uint16_t port = 443;
  bool tls = splitUrl(cfgBase, host, port, basePath);

  WiFiClientSecure secure;
  WiFiClient plain;
  Client* cli = nullptr;
  bool verify = false;
  if (tls) {
    verify = tlsVerifyEnabled();
    tlsConfigure(secure);
    secure.setTimeout(6000);
    secure.setHandshakeTimeout(8);
    cli = &secure;
  } else {
    plain.setTimeout(8000);
    cli = &plain;
  }

  String path = pathAndQuery.startsWith("/") ? pathAndQuery : ("/" + pathAndQuery);
  String req;
  req.reserve(420);
  req += String(method) + " " + path + " HTTP/1.1\r\n";
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

  unsigned long t0 = millis();
  static const size_t MAX_RESPONSE_BYTES = 4096;

  // Один обмен с конкретным адресом (byIp) или по имени хоста.
  // altHost позволяет обратиться к тому же серверу под вариантом имени —
  // так обходим кэш DNS, который в lwIP хранит только один A-адрес.
  // Возвращает HTTP-код, -2 при отказе соединения, -4 при слишком большом ответе.
  auto exchange = [&](bool byIp, const IPAddress& ip, const char* altHost) -> int {
    cli->stop();
    if (tls) {
      if (byIp) secure.setInsecure();   // по IP имя не проверить
      else tlsConfigure(secure);
      // mbedTLS с тремя CA и полными буферами просит ~45 КБ. Когда heap ниже,
      // проверку цепочки отключаем: разбор трёх сертификатов — самая дорогая
      // часть рукопожатия, а без неё плата хотя бы не молчит.
      if (ESP.getFreeHeap() < 62000) secure.setInsecure();
    }
    bool connected = byIp ? (cli->connect(ip, port) == 1)
                          : (cli->connect(altHost ? altHost : host.c_str(), port) == 1);
    if (!connected) {
      if (tls) tlsNoteFailure(secure);
      char err[80] = {0};
      if (tls) secure.lastError(err, sizeof(err));
      Serial.printf("[srv] connect fail %s:%u heap=%u max=%u tls=%d verify=%d err=%s\n",
                    host.c_str(), port, (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMaxAllocHeap(), tls ? 1 : 0,
                    (verify && !byIp) ? 1 : 0, err);
      cli->stop();
      delay(200);
      return -2;
    }
    tlsNoteSuccess(verify && !byIp);

    // Запоминаем адрес, с которым реально установилось соединение: имя имеет
    // два A-адреса, и один из них отдаёт 410 вместо API.
    if (tls && !haveGoodIp) {
      IPAddress rip = secure.remoteIP();
      if ((uint32_t)rip != 0) {
        goodIp = rip;
        haveGoodIp = true;
        if (prefsReady) prefs.putString("srvip", goodIp.toString());
        Serial.printf("[srv] рабочий адрес сервера: %s\n", goodIp.toString().c_str());
      }
    }

    cli->print(req);
    cli->flush();

    // Копим ответ в статическом буфере и только в конце делаем одну строку:
    // посимвольный += перестраивал буфер десятки раз и дробил heap.
    static char rbuf[MAX_RESPONSE_BYTES + 1];
    size_t rlen = 0;
    bool truncated = false;
    unsigned long deadline = millis() + 6000;
    while (millis() < deadline) {
      while (cli->available()) {
        int ch = cli->read();
        if (rlen < MAX_RESPONSE_BYTES) rbuf[rlen++] = (char)ch;
        else { truncated = true; break; }
      }
      if (truncated) break;
      if (!cli->connected() && !cli->available()) break;
      delay(1);
    }
    cli->stop();
    rbuf[rlen] = 0;
    response = rbuf;

    if (truncated) {
      Serial.printf("[srv] response exceeds %u bytes; discarded (heap=%u)\n",
                    (unsigned)MAX_RESPONSE_BYTES, (unsigned)ESP.getFreeHeap());
      response = "";
      return -4;
    }
    int c = -3;
    int sp = response.indexOf(' ');
    if (sp > 0) c = response.substring(sp + 1, sp + 4).toInt();
    return c;
  };

  // Порядок попыток: сначала проверенный адрес, затем полный список из DNS,
  // затем имя. По IP TLS не передаёт SNI и релей закрывает соединение, поэтому
  // по адресу ходим обычным HTTP на серверный plain-порт с заголовком Host —
  // сервер так и принимает устройства (проверено).
  int code = -2;
  if (!tls) {
    code = exchange(false, IPAddress(), nullptr);
  } else {
    if (haveGoodIp) {
      code = plainByIp(host, goodIp, req, response);
      if (!(code > 0 && code != 410)) haveGoodIp = false;
    }
    if (!haveGoodIp) {
      IPAddress addrs[4];
      int n = dnsQueryA(host, addrs, 4);
      for (int i = 0; i < n; i++) {
        int c2 = plainByIp(host, addrs[i], req, response);
        if (c2 > 0 && c2 != 410) {
          code = c2;
          goodIp = addrs[i];
          haveGoodIp = true;
          if (prefsReady) prefs.putString("srvip", goodIp.toString());
          Serial.printf("[srv] сервер через %s\n", goodIp.toString().c_str());
          break;
        }
        if (code < 0 && c2 > 0) code = c2;
        delay(150);
      }
      if (code <= 0 || code == 410) {
        int c3 = exchange(false, IPAddress(), nullptr);
        if (c3 > 0) code = c3;
      }
    }
  }

  Serial.printf("[srv] %s %s -> %d (%lu ms, heap=%u)\n", method, path.c_str(), code,
                (unsigned long)(millis() - t0), (unsigned)ESP.getFreeHeap());
  return code;
}

static void setStatus(const char* s);  // forward

#ifndef LEAF_PLAIN_PORT
#define LEAF_PLAIN_PORT 8090
#endif

// Обмен по обычному HTTP с конкретным адресом сервера (plain-порт 8090).
// Нужен потому, что TLS по IP теряет SNI и облачный релей закрывает соединение,
// а по имени половина запросов попадает на домашний роутер с 410 Gone.
static int plainByIp(const String& host, const IPAddress& ip, const String& req,
                     String& response) {
  WiFiClient c;
  c.setTimeout(8000);
  if (c.connect(ip, LEAF_PLAIN_PORT) != 1) { c.stop(); return -2; }
  c.print(req);
  c.flush();

  static char prb[2049];
  size_t rlen = 0;
  unsigned long deadline = millis() + 8000;
  while (millis() < deadline) {
    while (c.available()) {
      int ch = c.read();
      if (rlen < sizeof(prb) - 1) prb[rlen++] = (char)ch;
    }
    if (!c.connected() && !c.available()) break;
    delay(1);
  }
  c.stop();
  prb[rlen] = 0;
  response = prb;
  int sp = response.indexOf(' ');
  return sp > 0 ? response.substring(sp + 1, sp + 4).toInt() : -3;
}


// ---------------------------------------------------------------- SIM800 HTTP
// Наш TLS-сервер модуль не тянет (нет SNI), поэтому через симку ходим на
// обычный HTTP-порт сервера (проброс на роутере, по умолчанию 8090).
#ifndef LEAF_PLAIN_PORT
#define LEAF_PLAIN_PORT 8090
#endif

// Синхронизация через SIM800 включается, когда на роутере открыт проброс
// этого порта на сервер. До тех пор не тратим трафик и время на таймауты.
#ifndef LEAF_MODEM_HTTP
#define LEAF_MODEM_HTTP 1
#endif

static bool modemHttpReady() {
#if LEAF_MODEM_HTTP
  return modemDataReady();
#else
  return false;
#endif
}

static bool modemHttp(int method, const String& path, const char* body,
                      String& resp, int& code) {
  String host = String(cfgBase);
  int p = host.indexOf("://");
  if (p >= 0) host = host.substring(p + 3);
  p = host.indexOf('/');
  if (p >= 0) host = host.substring(0, p);
  // У AT+HTTP нельзя задать свой заголовок Authorization, поэтому токен платы
  // передаём параметром в URL (этот порт открыт только для /api/device/*).
  String full = path;
  full += (full.indexOf('?') >= 0) ? "&" : "?";
  full += "token=";
  full += cfgToken;
  String url = String("http://") + host + ":" + String(LEAF_PLAIN_PORT) + full;
  // Пока идёт HTTP, менеджер модема не должен слать свои AT (опрос вызовов),
  // иначе ответы перемешиваются и HTTPINIT/HTTPACTION ломаются.
  modemManagerPause(true);
  bool ok = modem800::hwHttp(method, url.c_str(), body, resp, code);
  modemManagerPause(false);
  return ok;
}

static bool postJson(const String& path, const char* body, String& resp) {
  int code;
  if (WiFi.status() != WL_CONNECTED) {
    if (!modemHttpReady()) return false;
    if (modemHttp(1, path, body, resp, code)) return true;
    if (code > 0) {
      Serial.printf("[srv] modem POST %s -> %d: %s\n", path.c_str(), code,
                    resp.substring(0, 120).c_str());
      if (code == 401 || code == 403) {
        saveToken("");
        registered = false;
        setStatus("re-enroll");
      }
    }
    return false;
  }
  code = request("POST", path, body, resp);
  if (code >= 200 && code < 300) return true;
  // WiFi associated but server unresponsive: try modem as fallback.
  if (code < 0 && modemHttpReady()) {
    if (modemHttp(1, path, body, resp, code)) return true;
  }
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

// Часы без Wi-Fi/NTP уходят в прошлое, и сервер отбрасывает точки истории
// как старые (ts <= последнего). Поэтому сверяем время по ответу сервера.
extern void setBoardEpoch(uint32_t epoch);

static void syncClockFrom(const String& resp) {
  int p = resp.indexOf("\"ts\":");
  if (p < 0) return;
  long ts = resp.substring(p + 5).toInt();
  if (ts < 1600000000L) return;
  long drift = (long)time(nullptr) - ts;
  if (drift > 120 || drift < -120) {
    setBoardEpoch((uint32_t)ts);
  }
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

  // Серийник для панели: заводской MAC чипа из eFuse — один и тот же с
  // рождения платы, не зависит от того, подключался ли Wi-Fi.
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  String mac(buf);

  char body[320];
  snprintf(body, sizeof(body),
           "{\"login\":\"%s\",\"password\":\"%s\",\"name\":\"Leaf\",\"serial\":\"%s\",\"fw\":\"%s\"}",
           cfgLogin, cfgPass, mac.c_str(), leafFwVersion);

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
  // Мощность считаем всегда, когда есть напряжение и ток: во время движения
  // packDataValid часто ещё не поднят, и график показывал ровный ноль.
  float packKw = (packV > 0 && fabsf(packI) > 0.1f) ? (packV * packI) / 1000.0f : 0.0f;
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

  char body[560];
  snprintf(body, sizeof(body),
           "{\"ts\":%lu,\"soc\":%.1f,\"odo\":%.0f,\"cabin\":%.1f,\"amb\":%.1f,"
           "\"batt\":%.1f,\"pack_v\":%.1f,\"pack_kw\":%.2f,\"v12\":%.1f,"
           "\"hvac\":%d,\"charging\":%d,\"locked\":%d,\"doors\":%d,\"soh\":%d,\"spd\":%.1f,"
           "\"bus_age_s\":%lu,\"profile\":\"%s\",\"profile_mode\":\"%s\"}",
           (unsigned long)ts,
           soc >= 0 ? soc : -1.0,
           (double)evData.odometerKm,
           cabinTemp, evData.ambientTemp,
           evData.batteryTemp, packV, packKw, 0.0,
           isHvacOn ? 1 : 0, (isChargingNow || tripLogIsCharging()) ? 1 : 0,
           (lockState == 1) ? 1 : 0,
           (doorFL || doorFR || doorRL || doorRR || doorTrunk) ? 1 : 0,
            evData.soh, (double)evSpeedKph(),
           (unsigned long)busAgeSeconds(), leafProfileName(), leafProfileModeName());

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
  char body[760];
  snprintf(body, sizeof(body),
           "{\"remote_mode\":%d,\"charge_limit\":%d,\"locking\":%d,\"sleep\":%d,\"sleep_after\":%d,"
           "\"wake_fast\":%d,\"wake_slow\":%d,\"uds\":%d,\"uds_tpms\":%d,"
           "\"modem\":%d,\"apn\":\"%s\",\"apn_user\":\"%s\",\"vehicle_profile\":%d,"
           "\"tariff_day\":%.2f,\"tariff_night\":%.2f,\"night_from\":%u,\"night_to\":%u,"
           "\"ssid\":\"%s\",\"ssid2\":\"%s\",\"ssid3\":\"%s\",\"pass_set\":[%d,%d,%d]}",
           (int)getRemoteTransport(),
           getChargeLimitPct(), getLockingEnabled() ? 1 : 0, getSleepEnabled() ? 1 : 0,
           getSleepAfterHours(), getWakeFastMinutes(), getWakeSlowMinutes(),
           getUdsEnabled() ? 1 : 0, getUdsTpmsEnabled() ? 1 : 0,
           getModemEnable() ? 1 : 0, getModem800Apn(), getModem800User(), getVehicleProfile(),
           tripLogTariffDay(), tripLogTariffNight(), tripLogNightFrom(), tripLogNightTo(),
           getWifiSSID(), getWifiSSID2(), getWifiSSID3(),
           (int)(strlen(getWifiPassword()) > 0),
           (int)(strlen(getWifiPassword2()) > 0),
           (int)(strlen(getWifiPassword3()) > 0));
  String resp;
  postJson("/api/device/config", body, resp);
}

static void applyConfig(const char* argsJson) {
  char key[24] = {0};
  if (!jsonString(argsJson, "\"key\"", key, sizeof(key))) return;

  // Число после "value": работает и для 8.0, и для "8.0" (строка в кавычках).
  auto valueDouble = [&]() -> double {
    const char* vp = strstr(argsJson, "\"value\"");
    if (!vp) return NAN;
    vp = strchr(vp, ':');
    if (!vp) return NAN;
    vp++;
    while (*vp == ' ' || *vp == '"') vp++;
    return atof(vp);
  };

  const char* vp = strstr(argsJson, "\"value\"");
  if (!vp) return;
  vp = strchr(vp, ':');
  if (!vp) return;
  vp++;
  while (*vp == ' ') vp++;
  int ival = atoi(vp);
  bool bval = (ival != 0) || (strncmp(vp, "true", 4) == 0);

  // Строковые настройки: значение приходит в кавычках ("host_name", "apn").
  {
    char sval[48];
    if (jsonString(argsJson, "\"value\"", sval, sizeof(sval)) && sval[0]) {
      if (!strcmp(key, "host_name")) {
        setHostName(sval);
        configDirty = true;
        Serial.printf("[srv] config host_name = %s\n", sval);
        return;
      }
      if (!strcmp(key, "apn")) {
        setModem800Apn(sval);
        configDirty = true;
        Serial.printf("[srv] config apn = %s\n", sval);
        return;
      }
      if (!strcmp(key, "apn_user")) {
        setModem800User(sval);
        configDirty = true;
        Serial.printf("[srv] config apn_user = %s\n", sval);
        return;
      }
      if (!strcmp(key, "apn_pass")) {
        setModem800Pass(sval);
        configDirty = true;
        Serial.println("[srv] config apn_pass updated");
        return;
      }
      // Wi-Fi сети (1..3): SSID задаётся строкой, пароль — только вместе с ним.
      if (!strcmp(key, "ssid")) {
        setWifiSSID(sval);
        configDirty = true;
        Serial.printf("[srv] config ssid = %s\n", sval);
        return;
      }
      if (!strcmp(key, "ssid2")) {
        setWifiSSID2(sval);
        configDirty = true;
        Serial.printf("[srv] config ssid2 = %s\n", sval);
        return;
      }
      if (!strcmp(key, "ssid3")) {
        setWifiSSID3(sval);
        configDirty = true;
        Serial.printf("[srv] config ssid3 = %s\n", sval);
        return;
      }
      if (!strcmp(key, "password")) {
        setWifiPassword(sval);
        configDirty = true;
        Serial.println("[srv] config password updated");
        return;
      }
      if (!strcmp(key, "password2")) {
        setWifiPassword2(sval);
        configDirty = true;
        Serial.println("[srv] config password2 updated");
        return;
      }
      if (!strcmp(key, "password3")) {
        setWifiPassword3(sval);
        configDirty = true;
        Serial.println("[srv] config password3 updated");
        return;
      }
    }
  }

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
  else if (!strcmp(key, "vehicle_profile")) setVehicleProfile(ival);
  else if (!strcmp(key, "tz_offset")) setTzOffset(ival);
  else if (!strcmp(key, "modem")) setModemEnable(bval);
  else if (!strcmp(key, "tariff_day") || !strcmp(key, "tariff_night") ||
           !strcmp(key, "night_from") || !strcmp(key, "night_to")) {
    // Тариф приходит с сервера (сохранённый в приложении) — применяем и
    // запоминаем в NVS, чтобы поездки считались в рублях.
    double d = valueDouble();
    if (isnan(d)) return;
    double day = tripLogTariffDay(), night = tripLogTariffNight();
    uint8_t from = tripLogNightFrom(), to = tripLogNightTo();
    if (!strcmp(key, "tariff_day")) day = d;
    else if (!strcmp(key, "tariff_night")) night = d;
    else if (!strcmp(key, "night_from")) from = (uint8_t)d;
    else to = (uint8_t)d;
    tripLogSetTariff(day, night, from, to);
  }
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

// Что делала последняя попытка OTA: уходит на сервер с sync-пакетом, чтобы
// причину отказа было видно в панели без serial-лога ("connect,h=45000" и т.п.).
static char otaStat[56] = "";

// Скачивает .bin с сервера и прошивает плату, затем перезагружается.
// Проверяет код ответа и длину: битый или неполный файл в flash не пишется.
static bool flashFromUrl(const char* url) {
  auto stat = [](const char* fmt, int v) {
    char buf[56];
    snprintf(buf, sizeof(buf), fmt, v);
    snprintf(otaStat, sizeof(otaStat), "%s,h=%u", buf, (unsigned)ESP.getFreeHeap());
    Serial.printf("[srv] ota: %s\n", otaStat);
  };
  // OTA качает только по Wi-Fi: через модем образ не потянуть, а без Wi-Fi
  // WiFiClientSecure может висеть вечно (watchdog на loop-задачу не натянут)
  // и плата замолкает до цикла питания. Лучше честно отказаться и уйти в nack.
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(otaStat, sizeof(otaStat), "nowifi");
    Serial.println("[srv] ota: no wifi, refuse");
    return false;
  }
  String host, path;
  uint16_t port = 443;
  bool tls = splitUrl(url, host, port, path);
  WiFiClientSecure secure;
  WiFiClient plain;
  Client* cli = tls ? (Client*)&secure : (Client*)&plain;
  bool verify = false;
  if (tls) {
    verify = tlsVerifyEnabled();
    tlsConfigure(secure);
    secure.setHandshakeTimeout(8);   // как в request(): без этого connect висит
  }
  bool connected = false;
  for (int attempt = 0; attempt < 2 && !connected; attempt++) {
    connected = (cli->connect(host.c_str(), port) == 1);
    if (!connected) { cli->stop(); delay(400); }
  }
  if (!connected) {
    if (tls) tlsNoteFailure(secure);
    char err[80] = {0};
    if (tls) secure.lastError(err, sizeof(err));
    snprintf(otaStat, sizeof(otaStat), "connect,err=%.20s,h=%u", err,
             (unsigned)ESP.getFreeHeap());
    Serial.printf("[srv] ota: connect failed heap=%u err=%s\n",
                  (unsigned)ESP.getFreeHeap(), err);
    return false;
  }
  tlsNoteSuccess(verify);
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
    stat("http%d", code);
    cli->stop();
    return false;
  }
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  Serial.printf("[srv] ota: image=%ld running=%s/%lu next=%s/%lu free=%u largest=%u\n",
                contentLen, running ? running->label : "none",
                running ? (unsigned long)running->size : 0UL,
                next ? next->label : "none",
                next ? (unsigned long)next->size : 0UL,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  if (!next || next == running) {
    snprintf(otaStat, sizeof(otaStat), "no-slot,h=%u", (unsigned)ESP.getFreeHeap());
    cli->stop();
    return false;
  }
  if (contentLen > 0 && (size_t)contentLen > next->size) {
    snprintf(otaStat, sizeof(otaStat), "oversize%ld/%lu", contentLen,
             (unsigned long)next->size);
    cli->stop();
    return false;
  }
  if (contentLen > 0) {
    if (!Update.begin((size_t)contentLen)) {
      stat("begin%ld", contentLen);
      Serial.printf("[srv] ota: Update.begin error=%u (%s), largest=%u\n",
                    (unsigned)Update.getError(), Update.errorString(),
                    (unsigned)ESP.getMaxAllocHeap());
      cli->stop();
      return false;
    }
  } else if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    stat("begin-1", 0);
    Serial.printf("[srv] ota: Update.begin error=%u (%s), largest=%u\n",
                  (unsigned)Update.getError(), Update.errorString(),
                  (unsigned)ESP.getMaxAllocHeap());
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
        stat("write%u", (int)total);
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
    snprintf(otaStat, sizeof(otaStat), "short%u/%ld", (unsigned)total, contentLen);
    Serial.printf("[srv] ota: %s\n", otaStat);
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    stat("end%u", (int)total);
    return false;
  }
  Serial.printf("[srv] ota: flashed %u bytes, rebooting\n", (unsigned)total);
  snprintf(otaStat, sizeof(otaStat), "ok%u", (unsigned)total);
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
  if (!postJson("/api/device/ping", body, resp)) return false;
  syncClockFrom(resp);
  return true;
}


// ---------------------------------------------------------------- история точек
// Плата копит телеметрию сама: при слабом Wi-Fi сервер получал по 4 точки за
// поездку, и графики превращались в прямые. Теперь точки уходят пачкой, как
// только появляется связь.
// ~43 min at 5 s cadence; on overflow downsample rather than dropping the
// beginning of a long charge. Keep RAM headroom for cellular TLS.
static const int HIST_N = 512;
struct HistPoint { uint32_t ts; int16_t soc10; int16_t kw10; int8_t batt; uint8_t spd; };
static HistPoint histBuf[HIST_N];
static int histCount = 0;
static uint32_t histLastTs = 0;
static uint32_t histMinInterval = 5;

void serverApiHistSample(float soc, float kw, float batt, float spd) {
  time_t tnow = time(nullptr);
  if (tnow < 1600000000) return;
  uint32_t ts = (uint32_t)tnow;
  if (ts < histLastTs || ts - histLastTs < histMinInterval) return;
  if (histCount >= HIST_N) {
    for (int j = 0; j < HIST_N / 2; ++j) histBuf[j] = histBuf[j * 2];
    histCount = HIST_N / 2;
    histMinInterval *= 2;
  }
  HistPoint& h = histBuf[histCount++];
  h.ts = ts;
  h.soc10 = (int16_t)lroundf(soc * 10.0f);
  h.kw10 = (int16_t)lroundf(kw * 10.0f);
  h.batt = (int8_t)lroundf(batt);
  h.spd = (uint8_t)(spd < 0 ? 0 : (spd > 250 ? 250 : lroundf(spd)));
  histLastTs = ts;
}

static bool uploadHistory() {
  if (histCount <= 0) return true;
  // Server accepts at most 500 points per request. Never acknowledge/drop the
  // tail of a longer offline buffer as though it had been persisted.
  const int batchCount = histCount < 400 ? histCount : 400;
  String body;
  body.reserve(batchCount * 34 + 24);
  body += "{\"points\":[";
  for (int i = 0; i < batchCount; i++) {
    const HistPoint& h = histBuf[i];
    if (i) body += ",";
    body += "[";
    body += String(h.ts);
    body += ",";
    body += String(h.soc10 / 10.0f, 1);
    body += ",";
    body += String(h.kw10 / 10.0f, 1);
    body += ",";
    body += String((int)h.batt);
    body += ",";
    body += String((int)h.spd);
    body += "]";
  }
  body += "]}";
  String resp;
  if (!postJson("/api/device/hist", body.c_str(), resp)) return false;
  Serial.printf("[srv] история: отправлено %d точек\n", batchCount);
  histCount -= batchCount;
  if (histCount > 0) memmove(histBuf, histBuf + batchCount, histCount * sizeof(HistPoint));
  else histMinInterval = 5;
  return true;
}

// ---------------------------------------------------------------- CAN-сессии
// Тестовый режим: плата пишет CAN во время поездок, климата и зарядки, затем
// выгружает файлы кусками по 192 КБ. Один кусок за цикл — команды с сервера
// не ждут конца многомегабайтной загрузки.
static char canUpName[40] = {0};
static size_t canUpTotal = 0;
static size_t canUpOff = 0;

static int requestBinary(const String& pathAndQuery, const char* file,
                         size_t off, size_t len, String& response) {
  String host, basePath;
  uint16_t port = 443;
  bool tls = splitUrl(cfgBase, host, port, basePath);

  // Свой клиент на каждый вызов: так mbedTLS освобождает и контекст, и
  // буферы, и heap возвращается к исходному. Постоянный клиент держал ~12 КБ
  // занятыми и оставлял TLS без непрерывного куска.
  WiFiClientSecure secure;
  WiFiClient plain;
  Client* cli = nullptr;
  bool verify = false;
  if (tls) {
    verify = tlsVerifyEnabled();
    tlsConfigure(secure);
    secure.setTimeout(20000);
    secure.setHandshakeTimeout(12);
    cli = &secure;
  } else {
    plain.setTimeout(20000);
    cli = &plain;
  }
  cli->stop();
  if (cli->connect(host.c_str(), port) != 1) {
    if (tls) tlsNoteFailure(secure);
    cli->stop();
    return -2;
  }
  tlsNoteSuccess(verify);

  String path = pathAndQuery.startsWith("/") ? pathAndQuery : ("/" + pathAndQuery);
  String req;
  req.reserve(320);
  req += "POST " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "User-Agent: LeafControl\r\n";
  req += "Connection: close\r\n";
  req += "Content-Type: application/octet-stream\r\n";
  req += String("Authorization: Bearer ") + cfgToken + "\r\n";
  req += "Content-Length: " + String((unsigned long)len) + "\r\n\r\n";
  cli->print(req);
  cli->flush();

  static uint8_t buf[1024];
  size_t sent = 0;
  while (sent < len) {
    size_t want = len - sent;
    if (want > sizeof(buf)) want = sizeof(buf);
    size_t got = canLoggerReadAt(file, off + sent, buf, want);
    if (!got) { cli->stop(); return -5; }
    if (cli->write(buf, got) != got) { cli->stop(); return -6; }
    sent += got;
    delay(0);
  }
  cli->flush();

  response = "";
  unsigned long deadline = millis() + 25000;
  while (millis() < deadline) {
    while (cli->available()) {
      if (response.length() < 512) response += (char)cli->read();
      else (void)cli->read();
    }
    if (!cli->connected() && !cli->available()) break;
    delay(1);
  }
  cli->stop();

  int code = -3;
  int sp = response.indexOf(' ');
  if (sp > 0) code = response.substring(sp + 1, sp + 4).toInt();
  return code;
}

static void uploadCanSession() {
  if (cfgToken[0] == '\0') return;
  // Файл могли удалить (мало места) — очередь пересоберём заново.
  if (canUpName[0] != '\0' && !canLoggerExists(canUpName)) {
    Serial.printf("[srv] CAN %s исчез, откладываю\n", canUpName);
    canUpName[0] = '\0';
    canUpOff = 0;
    canUpTotal = 0;
  }
  if (canUpName[0] == '\0') {
    size_t total = 0;
    if (!canLoggerNextPending(canUpName, sizeof(canUpName), &total)) return;
    canUpTotal = total;
    canUpOff = 0;
    if (canUpTotal == 0) {
      canLoggerMarkSent(canUpName);
      canUpName[0] = '\0';
      return;
    }
    Serial.printf("[srv] CAN-сессия %s (%lu КБ)\n", canUpName,
                  (unsigned long)(canUpTotal / 1024));
  }

  static const size_t CHUNK = 192UL * 1024UL;
  size_t len = canUpTotal - canUpOff;
  if (len > CHUNK) len = CHUNK;
  bool last = (canUpOff + len >= canUpTotal);

  String q = "/api/device/canlog?name=";
  q += canUpName;
  q += "&off=" + String((unsigned long)canUpOff);
  q += "&total=" + String((unsigned long)canUpTotal);
  if (last) q += "&last=1";

  String resp;
  int code = requestBinary(q, canUpName, canUpOff, len, resp);
  if (code >= 200 && code < 300) {
    canUpOff += len;
    if (canUpOff >= canUpTotal) {
      Serial.printf("[srv] CAN-сессия %s отправлена\n", canUpName);
      canLoggerMarkSent(canUpName);
      canUpName[0] = '\0';
      canUpOff = 0;
      canUpTotal = 0;
    }
  } else {
    Serial.printf("[srv] CAN %s @%lu -> %d %s\n", canUpName,
                  (unsigned long)canUpOff, code, resp.substring(0, 120).c_str());
    if (code < 0) canUpOff = 0;  // обрыв — файл начинаем заново
    if (code == 401 || code == 403) {
      saveToken("");
      registered = false;
      setStatus("re-enroll");
      canUpName[0] = '\0';
      canUpOff = 0;
    }
  }
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
  if (otaStat[0]) {
    payload += ",\"ota\":\"";
    payload += otaStat;
    payload += "\"";
  }

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
  syncClockFrom(resp);

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

  // Сервер — источник истины для кода привязки: если он выдал новый код
  // (например, после отвязки), плата должна показывать именно его.
  {
    char cc[24];
    if (jsonString(resp, "\"claim_code\"", cc, sizeof(cc)) && cc[0] &&
        strcmp(cc, claimCode) != 0) {
      snprintf(claimCode, sizeof(claimCode), "%s", cc);
      Serial.printf("[srv] claim code updated: %s\n", claimCode);
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
  if (WiFi.status() != WL_CONNECTED) {
    if (!modemHttpReady()) return false;
    int mcode = 0;
    if (!modemHttp(0, "/api/device/cmd?wait=0", nullptr, resp, mcode)) return false;
    handleCommandResponse(resp);
    return true;
  }
  int code = request("GET", "/api/device/cmd?wait=0", nullptr, resp, 6000);
  if (code >= 200 && code < 300) {
    handleCommandResponse(resp);
    return true;
  }
  // WiFi associated but server unreachable: modem is already attached in
  // this state (server-based wifiHasNet), so poll over cellular too.
  if (code < 0 && modemHttpReady()) {
    String mresp;
    int mcode = 0;
    if (modemHttp(0, "/api/device/cmd?wait=0", nullptr, mresp, mcode)) {
      handleCommandResponse(mresp);
      return true;
    }
  }
  Serial.printf("[srv] poll -> %d %s\n", code, resp.substring(0, 120).c_str());
  return false;
}

// ---------------------------------------------------------------- Р·Р°РїРёСЃРё

void serverApiOnRecord(const char* kind, const char* line) {
  if (!cfgEnabled) return;
  // Drain oldest entries when queue is full - never drop a finished session.
  while (recCount >= REC_QUEUE) {
    recHead = (recHead + 1) % REC_QUEUE;
    recCount--;
  }
  snprintf(recQueue[recTail], sizeof(recQueue[recTail]), "%s", line);
  snprintf(recKind[recTail], sizeof(recKind[recTail]), "%s", kind);
  recTail = (recTail + 1) % REC_QUEUE;
  recCount++;
}

// РџР»Р°С‚Р° С…СЂР°РЅРёС‚ СЃРІРѕРё Р·Р°РїРёСЃРё РєР°Рє {"s":start,"e":end,"d":СЃРµРє,"k":РєРј,"w":Wh,"s0":..,"s1":..}
// РЎРµСЂРІРµСЂ Р¶РґС‘С‚ {kind,started_at,ended_at,duration_s,distance_km,energy_kwh,soc_start,soc_end}
static bool sendOneRecord() {
  char diskLine[220], diskKind[8];
  bool disk = tripLogNextReplay(diskKind, sizeof(diskKind), diskLine, sizeof(diskLine));
  bool queued = !disk && recCount > 0;
  if (!disk && !queued) return false;
  const char* line = queued ? recQueue[recHead] : diskLine;
  const char* kind = queued ? recKind[recHead] : diskKind;

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
           (strcmp(kind, "charge") == 0) ? "charge"
                                         : (strcmp(kind, "climate") == 0 ? "climate" : "trip"),
           (unsigned long)s, (unsigned long)e, (unsigned long)d,
           km, wh / 1000.0, s0, s1);

  String resp;
  if (postJson("/api/device/session", body, resp)) {
    Serial.printf("[srv] Р·Р°РїРёСЃСЊ %s РѕС‚РїСЂР°РІР»РµРЅР°\n", kind);
    tripLogNoteRecordSent((uint32_t)e);
    if (queued) {
      recHead = (recHead + 1) % REC_QUEUE;
      recCount--;
    } else tripLogReplayConfirmed();
    return true;
  }
  // Leave the queue head / replay cursor untouched until acknowledged.
  return false;
}

// ---------------------------------------------------------------- API

void serverApiBegin() {
  loadConfig();
  Serial.printf("[srv] СЃРµСЂРІРµСЂ: %s, СѓС‡С‘С‚РєР°: %s, %s\n", cfgBase, cfgLogin,
                cfgEnabled ? "РІРєР»СЋС‡РµРЅРѕ" : "РІС‹РєР»СЋС‡РµРЅРѕ");
}

void serverApiApplyConfig(const char* argsJson) { applyConfig(argsJson); }

void serverApiSetCommandHandler(ServerCommandHandler handler) { cmdHandler = handler; }
const char* serverApiLogin() { return cfgLogin; }
const char* serverApiPassword() { return cfgPass; }
const char* serverApiStatusText() { return statusText; }
const char* serverApiBase() { return cfgBase; }
// Сменить адрес сервера (serial CLI "set base <url>"). Сохраняем в NVS
// ("leafsrv"/"base") и сразу применяем: следующий sync пойдёт на новый адрес.
void serverApiSetBase(const char* base) {
  snprintf(cfgBase, sizeof(cfgBase), "%s", base);
  Preferences prefs;
  if (prefs.begin("leafsrv", false)) {
    prefs.putString("base", base);
    prefs.end();
  }
  Serial.printf("[srv] base = %s\n", cfgBase);
}
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
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  if (!wifiUp && !modemHttpReady()) {
    setStatus("no link");
    return;
  }
  if (getRemoteTransport() != RemoteTransport::SERVER) return;
  unsigned long now = millis();

  static uint8_t failCount = 0;
  static unsigned long retryAt = 0;
  // Full telemetry may be in backoff; persisted trip delivery has its own
  // retry path below and must not be held behind that backoff.
  bool syncBackoff = failCount >= 3 && (long)(now - retryAt) < 0;

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
  // По симке экономим трафик: полная синхронизация раз в минуту.
  unsigned long interval = wifiUp ? ((WiFi.RSSI() < -80) ? 15000UL : 8000UL) : 60000UL;
  static bool linkOk = false;
  bool syncOk = false;
  if (!syncBackoff && now - lastSyncMs >= interval) {
    lastSyncMs = now;
    syncOk = syncRequest();
    linkOk = syncOk;
    if (syncOk) {
      lastOkMs = millis();
      failCount = 0;
      // накопленные точки телеметрии — пачкой, не чаще раза в минуту
      static unsigned long lastHistMs = 0;
      if (histCount > 0 && now - lastHistMs > 60000UL) {
        lastHistMs = now;
        uploadHistory();
      }
      // CAN-сессии для разбора: по одному куску за цикл.
      if (canLoggerPendingCount() > 0) uploadCanSession();
    } else if (failCount < 255 && ++failCount >= 3) {
      retryAt = now + 30000UL;
    }
  }

  // Delivery must not wait for a successful full telemetry sync: a finished
  // trip is sent as soon as the network task can retry it.
  static unsigned long lastRecordAttemptMs = 0;
  if (registered && cfgToken[0] && now - lastRecordAttemptMs >= 5000UL) {
    lastRecordAttemptMs = now;
    // Drain a few records per tick instead of one (bounded so a large
    // backlog cannot starve telemetry for minutes on the slow modem link).
    for (int n = 0; n < 4 && sendOneRecord(); n++) {}
  }

  // Раз в минуту (и сразу после неудачной синхронизации) отправляем короткий
  // ping: сервер обновляет last_seen и версию прошивки.
  static unsigned long lastPingMs = 0;
  bool pingDue = (lastPingMs == 0) || (now - lastPingMs >= 60000UL) ||
                 (!linkOk && (now - lastPingMs >= 20000UL));
  if (pingDue && (wifiUp || modemHttpReady())) {
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
    if (WiFi.status() == WL_CONNECTED || modemHttpReady()) {
      serverNetOnce();
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

unsigned long serverApiLastOkMs() { return lastOkMs; }

void serverApiTick() {
  static bool started = false;
  if (started) return;
  if (xTaskCreatePinnedToCore(serverNetLoop, "leaf-net", 12288, nullptr, 1, nullptr, 0) == pdPASS) {
    started = true;
    Serial.println("[srv] сетевой поток запущен");
  } else {
    Serial.println("[srv] ERROR: не удалось запустить сетевой поток");
  }
}
