#include "tripLog.h"

extern void serverApiOnRecord(const char* kind, const char* line);
extern void serverApiHistSample(float soc, float kw, float batt, float spd);
#include "boardConfig.h"
#if defined(LEAF_BOARD_TCALL) && (LEAF_BOARD_TCALL)
#include <SPIFFS.h>
#define HISTFS SPIFFS
#else
#include <FFat.h>
#define HISTFS FFat
#endif
#include <WebServer.h>
#include <time.h>
#include <Preferences.h>
#include "vehicleTypes.h"
#include "evCanLeaf.h"

// Board-side history: the phone can be closed, the board keeps counting. One
// JSON line per finished trip / charge session in the FAT partition.
//
// Sizing: a record is ~110-150 bytes, so 1 MB holds roughly 6000 trips and
// 256 KB holds roughly 1600 charge sessions. When a file grows past its limit
// the older half is dropped (see rotateFile()).

extern EvBatteryData evData;
extern bool isChargingNow;
extern float currentSOC;
extern void requestVehicleWake();
extern WebServer server;

static const char* TRIPS_PATH   = "/trips.jsonl";
static const char* CHARGES_PATH = "/charges.jsonl";
static const char* CLIMATE_PATH = "/climate.jsonl";
static const size_t TRIPS_LIMIT   = BOARD_TRIPS_LIMIT;
static const size_t CHARGES_LIMIT = BOARD_CHARGES_LIMIT;
static const size_t CLIMATE_LIMIT = 64UL * 1024UL;
static const size_t SEND_MAX_RECORDS = 800;

// Moscow, no DST. The board stores UTC epochs; localtime_r() on this core does
// not always apply the configTime() offset, so the hour is calculated directly.
static const uint32_t TZ_OFFSET_SEC = 3UL * 3600UL;
static uint8_t nightFrom = 0;   // 00:00
static uint8_t nightTo = 7;     // 07:00
static float tariffDay = 0.0f;
static float tariffNight = 0.0f;

static unsigned long lastTickMs = 0;
static float lastOdo = -1.0f;

static size_t tripsCount = 0;
static size_t chargesCount = 0;
static size_t climateCount = 0;

// До какого времени записи уже ушли на сервер. Нужен, чтобы после
// перезагрузки/сна дослать то, что не успело отправиться.
static uint32_t lastSentEpoch = 0;

// --- trip state ----------------------------------------------------------
static bool tripActive = false;
static uint32_t tripStartEpoch = 0;
static unsigned long tripStartMs = 0;
static float tripOdoStart = 0;
static float tripSocStart = -1;
static float tripSocEnd = -1;
static float tripKwh = 0, tripKwhDay = 0, tripKwhNight = 0;
static unsigned long tripMoveMs = 0;      // последнее движение
static unsigned long tripOffSinceMs = 0;  // с какого момента машина «не готова»
static bool tripPending = false;          // едем, но ещё меньше 100 м
static uint32_t tripPendEpoch = 0;
static unsigned long tripPendMs = 0;
static float tripPendOdo = 0;
static float tripPendSoc = -1;

// Признак «машина живёт» для авто-записи CAN: включена, едет, климат, зарядка.
static bool sessionActive = false;

// --- климат (машина включена, стоит, потребляет) -------------------------
static bool climActive = false;static uint32_t climStartEpoch = 0;
static unsigned long climStartMs = 0;
static float climWh = 0, climWhDay = 0, climWhNight = 0;
static float climSocStart = -1, climSocEnd = -1;

// --- charge state --------------------------------------------------------
static bool chgActive = false;
static uint32_t chgStartEpoch = 0;
static unsigned long chgStartMs = 0;
static float chgSocStart = -1;
static float chgSocEnd = -1;
static float chgKwh = 0, chgKwhDay = 0, chgKwhNight = 0;

static void resendUnsent();  // досылка записей после перезагрузки

// Состояние открытой поездки в RTC: переживает перезагрузку и сон, чтобы
// запись не пропадала, если плата ушла в ребут посреди поездки.
RTC_DATA_ATTR static uint8_t rtcTripActive = 0;
RTC_DATA_ATTR static uint32_t rtcTripStart = 0;
RTC_DATA_ATTR static float rtcTripOdoStart = 0;
RTC_DATA_ATTR static float rtcTripOdoLast = 0;
RTC_DATA_ATTR static float rtcTripSocStart = -1;
RTC_DATA_ATTR static float rtcTripKwh = 0;

static uint32_t epochNow() {
  time_t t = time(nullptr);
  return (t > 1600000000) ? (uint32_t)t : 0;
}

// The board stores UTC epochs. Convert to the car's local hour explicitly and
// apply the user's night window (which may wrap over midnight).
static bool isNight(uint32_t epoch) {
  if (!epoch) return false;
  int h = (int)(((epoch + TZ_OFFSET_SEC) / 3600UL) % 24UL);
  if (nightFrom == nightTo) return false;
  return (nightFrom < nightTo) ? (h >= nightFrom && h < nightTo)
                               : (h >= nightFrom || h < nightTo);
}

static size_t countLines(File& f) {
  size_t lines = 0;
  while (f.available()) {
    if (f.read() == '\n') lines++;
  }
  return lines;
}

static size_t fileLines(const char* path) {
  File f = HISTFS.open(path, FILE_READ);
  if (!f) return 0;
  size_t n = countLines(f);
  f.close();
  return n;
}

static void rotateFile(const char* path, size_t limitBytes) {
  File in = HISTFS.open(path, FILE_READ);
  if (!in) return;
  size_t size = in.size();
  if (size <= limitBytes) { in.close(); return; }

  // Skip to the middle, then to the next newline so the file keeps whole lines.
  in.seek(size / 2);
  while (in.available() && in.read() != '\n') {}
  File out = HISTFS.open("/tmp.jsonl", FILE_WRITE);
  if (!out) { in.close(); return; }
  uint8_t buf[512];
  while (in.available()) {
    size_t n = in.read(buf, sizeof(buf));
    if (!n) break;
    out.write(buf, n);
    delay(0);
  }
  out.close();
  in.close();
  HISTFS.remove(path);
  HISTFS.rename("/tmp.jsonl", path);
  size_t kept = fileLines(path);
  if (strcmp(path, TRIPS_PATH) == 0) tripsCount = kept;
  else chargesCount = kept;
  Serial.printf("[log] rotated %s (was %u bytes, kept %u lines)\n",
                path, (unsigned)size, (unsigned)kept);
}

static void appendRecord(const char* path, size_t limitBytes, const char* line) {
  File f = HISTFS.open(path, FILE_APPEND);
  if (!f) {
    Serial.printf("[log] cannot open %s\n", path);
    return;
  }
  f.print(line);
  f.print('\n');
  f.close();
  if (strcmp(path, TRIPS_PATH) == 0) tripsCount++;
  else chargesCount++;
  rotateFile(path, limitBytes);
}

// One-time cleanup for phantom charge records created by the early EV-CAN
// detector (5-15 s current spikes during regen). A real charge is longer than
// two minutes; the user's 2.5-hour session is kept.
static void compactCharges() {
  File in = HISTFS.open(CHARGES_PATH, FILE_READ);
  if (!in) return;
  File out = HISTFS.open("/charges.tmp", FILE_WRITE);
  if (!out) { in.close(); return; }
  int removed = 0;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    int d = 0;
    int p = line.indexOf("\"d\":");
    if (p >= 0) d = line.substring(p + 4).toInt();
    if (d > 0 && d < 120) { removed++; continue; }
    out.print(line);
    out.print('\n');
  }
  out.close();
  in.close();
  if (removed > 0) {
    HISTFS.remove(CHARGES_PATH);
    HISTFS.rename("/charges.tmp", CHARGES_PATH);
    Serial.printf("[log] removed %d short charge records\n", removed);
  } else {
    HISTFS.remove("/charges.tmp");
  }
}

void tripLogInit() {
  // T-Call has a small SPIFFS partition and may need the initial format;
  // the T-2CAN FAT partition must never be formatted here because canLogger
  // shares it.
  Preferences prefs;
  if (prefs.begin("leafrec", false)) {
    lastSentEpoch = prefs.getULong("sent", 0);
    prefs.end();
  }
  if (prefs.begin("leaftariff", false)) {
    nightFrom = prefs.getUChar("from", 0);
    nightTo = prefs.getUChar("to", 7);
    tariffDay = prefs.isKey("day") ? prefs.getFloat("day", 0.0f) : 0.0f;
    tariffNight = prefs.isKey("night") ? prefs.getFloat("night", 0.0f) : 0.0f;
    if (nightFrom > 23) nightFrom = 0;
    if (nightTo > 23) nightTo = 7;
    prefs.end();
  }
  #if defined(LEAF_BOARD_TCALL) && (LEAF_BOARD_TCALL)
  if (!HISTFS.begin(true)) {
#else
  if (!HISTFS.begin(false)) {
#endif
    Serial.println("[log] history filesystem unavailable, history is disabled");
    return;
  }
  tripsCount = fileLines(TRIPS_PATH);
  compactCharges();
  chargesCount = fileLines(CHARGES_PATH);
  climateCount = fileLines(CLIMATE_PATH);
  Serial.printf("[log] history on board: %u trips, %u charges\n",
                (unsigned)tripsCount, (unsigned)chargesCount);
  resendUnsent();

  // Плата перезагрузилась посреди поездки — записываем то, что успели накопить.
  if (rtcTripActive) {
    rtcTripActive = 0;
    uint32_t endEpoch = epochNow();
    float km = (rtcTripOdoLast > 0 && rtcTripOdoStart > 0) ? (rtcTripOdoLast - rtcTripOdoStart) : 0;
    if (km < 0) km = 0;
    if (rtcTripStart > 0 && km >= 0.05f) {
      char line[200];
      snprintf(line, sizeof(line),
               "{\"s\":%lu,\"e\":%lu,\"d\":%lu,\"k\":%.2f,\"w\":%.3f,\"wd\":%.3f,\"wn\":%.3f,\"s0\":%d,\"s1\":%d,\"r\":\"restored\"}",
               (unsigned long)rtcTripStart, (unsigned long)(endEpoch ? endEpoch : rtcTripStart),
               (unsigned long)((endEpoch > rtcTripStart) ? (endEpoch - rtcTripStart) : 0),
               km, rtcTripKwh, rtcTripKwh, 0.0f,
               (int)rtcTripSocStart, -1);
      appendRecord(TRIPS_PATH, TRIPS_LIMIT, line);
      serverApiOnRecord("trip", line);
      Serial.printf("[log] restored trip %.2f km after reboot\n", km);
    }
  }
}

static void saveTripToRtc(bool active) {
  rtcTripActive = active ? 1 : 0;
  if (!active) return;
  rtcTripStart = tripStartEpoch;
  rtcTripOdoStart = tripOdoStart;
  rtcTripOdoLast = lastOdo;
  rtcTripSocStart = tripSocStart;
  rtcTripKwh = tripKwh;
}

static void finishTrip(const char* reason) {
  if (!tripActive) return;
  tripActive = false;
  saveTripToRtc(false);
  uint32_t endEpoch = epochNow();
  unsigned long durMs = millis() - tripStartMs;
  float km = (lastOdo > 0 && tripOdoStart > 0) ? (lastOdo - tripOdoStart) : 0;
  if (km < 0) km = 0;
  // Ignore noise: shorter than 20 s and less than 50 m.
  if (durMs < 20000UL && km < 0.05f) return;

  char line[200];
  // "w"/"wd"/"wn" are watt-hours (Wh), not kWh. The PWA stores Wh directly.
  snprintf(line, sizeof(line),
           "{\"s\":%lu,\"e\":%lu,\"d\":%lu,\"k\":%.2f,\"w\":%.3f,\"wd\":%.3f,\"wn\":%.3f,\"s0\":%d,\"s1\":%d,\"r\":\"%s\"}",
           (unsigned long)tripStartEpoch, (unsigned long)endEpoch,
           (unsigned long)(durMs / 1000UL), km, tripKwh, tripKwhDay, tripKwhNight,
           (int)(tripSocStart >= 0 ? tripSocStart : -1),
           (int)(tripSocEnd >= 0 ? tripSocEnd : -1), reason);
  appendRecord(TRIPS_PATH, TRIPS_LIMIT, line);
  // На сервер отправляем независимо от файловой системы: запись создаётся
  // в памяти и уходит в очередь serverApi, даже если SPIFFS недоступен.
  serverApiOnRecord("trip", line);
  Serial.printf("[log] trip %.2f km, %.2f kWh\n", km, tripKwh);
}

static void finishCharge(const char* reason) {
  if (!chgActive) return;
  chgActive = false;
  uint32_t endEpoch = epochNow();
  unsigned long durMs = millis() - chgStartMs;
  // Reject phantom sessions from short current spikes (regen, contactor
  // noise). A real charge lasts at least two minutes and stores >100 Wh.
  if (durMs < 120000UL || chgKwh < 100.0f) return;

  char line[200];
  // "w"/"wd"/"wn" are watt-hours (Wh), not kWh. The PWA stores Wh directly.
  snprintf(line, sizeof(line),
           "{\"s\":%lu,\"e\":%lu,\"d\":%lu,\"w\":%.3f,\"wd\":%.3f,\"wn\":%.3f,\"s0\":%d,\"s1\":%d,\"r\":\"%s\"}",
           (unsigned long)chgStartEpoch, (unsigned long)endEpoch,
           (unsigned long)(durMs / 1000UL), chgKwh, chgKwhDay, chgKwhNight,
           (int)(chgSocStart >= 0 ? chgSocStart : -1),
           (int)(chgSocEnd >= 0 ? chgSocEnd : -1), reason);
  appendRecord(CHARGES_PATH, CHARGES_LIMIT, line);
  serverApiOnRecord("charge", line);
  Serial.printf("[log] charge %.2f kWh\n", chgKwh);
}

static void finishClimate(const char* reason) {
  if (!climActive) return;
  climActive = false;
  uint32_t endEpoch = epochNow();
  unsigned long durMs = millis() - climStartMs;
  // Климат — это заметная работа: минимум 5 минут и 100 Вт·ч.
  if (durMs < 300000UL || climWh < 100.0f) {
    Serial.printf("[log] climate session dropped (%.1f Wh, %lu s)\n",
                  climWh, (unsigned long)(durMs / 1000UL));
    return;
  }
  char line[200];
  snprintf(line, sizeof(line),
           "{\"s\":%lu,\"e\":%lu,\"d\":%lu,\"w\":%.3f,\"wd\":%.3f,\"wn\":%.3f,\"s0\":%d,\"s1\":%d,\"r\":\"%s\"}",
           (unsigned long)climStartEpoch, (unsigned long)endEpoch,
           (unsigned long)(durMs / 1000UL), climWh, climWhDay, climWhNight,
           (int)(climSocStart >= 0 ? climSocStart : -1),
           (int)(climSocEnd >= 0 ? climSocEnd : -1), reason);
  appendRecord(CLIMATE_PATH, CLIMATE_LIMIT, line);
  serverApiOnRecord("climate", line);
  Serial.printf("[log] climate %.2f kWh, %lu min\n", climWh / 1000.0, (unsigned long)(durMs / 60000UL));
}

void tripLogNoteRecordSent(uint32_t endEpoch) {
  if (endEpoch == 0 || endEpoch <= lastSentEpoch) return;
  lastSentEpoch = endEpoch;
  Preferences prefs;
  if (prefs.begin("leafrec", false)) {
    prefs.putULong("sent", lastSentEpoch);
    prefs.end();
  }
}

// После старта досылаем записи, которые не успели уйти (были офлайн или спали).
static void resendUnsent() {
  if (lastSentEpoch == 0) return;  // первый запуск: не спамим историей
  const char* paths[3] = {TRIPS_PATH, CHARGES_PATH, CLIMATE_PATH};
  const char* kinds[3] = {"trip", "charge", "climate"};
  int queued = 0;
  for (uint8_t k = 0; k < 3 && queued < 16; k++) {
    File f = HISTFS.open(paths[k], FILE_READ);
    if (!f) continue;
    while (f.available() && queued < 16) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() < 8) continue;
      int p = line.indexOf("\"e\":");
      if (p < 0) continue;
      uint32_t e = (uint32_t)line.substring(p + 4).toInt();
      if (e > lastSentEpoch) {
        serverApiOnRecord(kinds[k], line.c_str());
        queued++;
      }
    }
    f.close();
  }
  if (queued) Serial.printf("[log] re-queued %d unsent records\n", queued);
}

void tripLogTick() {
  unsigned long now = millis();
  if (now - lastTickMs < 5000UL) return;
  unsigned long dtMs = lastTickMs ? (now - lastTickMs) : 0;
  lastTickMs = now;

  float odo = (float)evData.odometerKm;
  float soc = evData.instrumentSOC >= 0 ? evData.instrumentSOC
                                        : (currentSOC >= 0 ? currentSOC : -1);
  float v = evData.packVoltage;
  float i = evData.packCurrent;
  bool dataOk = evData.packDataValid && evData.packVoltageValid;
  bool odoMoving = (odo > 0 && lastOdo > 0 && odo > lastOdo + 0.02f);
  // The CAR-CAN can be asleep during a charge, and the vehicleOn flag is
  // useless here: this car reports "on" (8) while charging too, which used
  // to block the detector for the whole session. The honest plugged-in
  // signal lives on EV-CAN itself: 0x1DC's charge current limit is > 0
  // exactly while a charger is connected and 0 while driving, so regen
  // phantom charges stay impossible even with a frozen odometer.
  float chargeLim = evCanChargeLimitAmps();
  bool pluggedIn = (chargeLim > 0.0f);
  bool evChargeRaw = dataOk && v > 100.0f && i < -0.5f &&
                     pluggedIn && !odoMoving;
  static uint8_t evSeen = 0;
  static uint8_t evStop = 0;
  if (evChargeRaw) {
    if (evSeen < 12) evSeen++;
    evStop = 0;
  } else {
    evSeen = 0;
    if (chgActive && evStop < 3) evStop++;
    else if (!chgActive) evStop = 0;
  }
  bool evHolding = chgActive && evStop < 3;
  bool evCharging = (evChargeRaw && evSeen >= 12) || evHolding;
  bool charging = isChargingNow || evCharging;
  uint32_t epoch = epochNow();

  // Wake the CAR-CAN gateway read-only when EV-CAN shows a charge but the
  // gateway has not reported it yet. This brings SOC/charger state early.
  static unsigned long lastChargeWakeMs = 0;
  if (evCharging && !isChargingNow && (lastChargeWakeMs == 0 ||
      now - lastChargeWakeMs > 300000UL)) {
    lastChargeWakeMs = now;
    Serial.println("[log] EV-CAN shows charging, requesting CAR-CAN wake");
    requestVehicleWake();
  }

  // Energy of the interval, split by local time for the day/night tariff.
  float wh = 0;
  if (dtMs > 0 && dtMs < 30000UL && dataOk && v > 0) {
    wh = fabsf(v * i) * (dtMs / 3600000.0f);
  }

  // Movement is the only honest trip trigger: vehicleOn on this car often
  // stays 0/-1, and relying on it made the board skip whole drives.
  bool moved = odoMoving;

  // ---- поездки -----------------------------------------------------------
  // Поездка начинается после 100 м хода и заканчивается только когда машина
  // выключена. Остановки в пути (с включённой машиной) её больше не делят:
  // раньше поездку рвало через 3 минуты стоянки, отсюда «1 км + 16 км».
  bool ready = (evData.vehicleOn == 1);
  float powerW = (dataOk && v > 0) ? fabsf(v * i) : 0.0f;

  if (!tripActive) {
    // Одометр машины отдаёт целые километры, поэтому первое же новое значение
    // — это уже полноценная поездка. Раньше старт требовал второго километра
    // в пределах двух минут: в городе километр набегает дольше, и поездки
    // терялись целиком (12.11.2026 не записалась ни одна).
    if (moved && epoch && lastOdo > 0) {
      tripActive = true;
      tripPending = false;
      tripStartEpoch = epoch;
      tripStartMs = now;
      tripOdoStart = lastOdo;
      tripSocStart = soc;
      tripSocEnd = soc;
      tripKwh = tripKwhDay = tripKwhNight = 0;
      tripMoveMs = now;
      tripOffSinceMs = 0;
      saveTripToRtc(true);
      Serial.printf("[log] trip started at %.0f km (first new km)\n", (double)tripOdoStart);
    } else if (tripPending && (now - tripPendMs > 120000UL)) {
      tripPending = false;
    }
  }

  if (tripActive) {
    if (moved) {
      tripMoveMs = now;
      tripOffSinceMs = 0;
      rtcTripOdoLast = lastOdo;
    }
    if (soc >= 0) tripSocEnd = soc;
    if (wh > 0) {
      tripKwh += wh;
      if (isNight(epoch)) tripKwhNight += wh; else tripKwhDay += wh;
      rtcTripKwh = tripKwh;
    }
    // Конец поездки: машина не в READY, не едет и не заряжается. Пять минут
    // запаса — чтобы стоянка у магазина не закрывала поездку.
    // Машина выключена = не READY, не едет, не заряжается и не тянет ток.
    if (!ready && !moved && !charging && powerW < 150.0f) {
      if (tripOffSinceMs == 0) tripOffSinceMs = now;
    } else {
      tripOffSinceMs = 0;
    }
    // Страховка: шина молчит 15 минут — машина точно спит.
    bool busSilent = evCanLastFrameAgeSec() > 900UL;
    if ((tripOffSinceMs != 0 && now - tripOffSinceMs > 480000UL) || busSilent) {
      finishTrip("off");
    }
  }

  // ---- климат ------------------------------------------------------------
  // Машина включена, стоит и тянет ток — это климат, а не поездка.
  bool idling = ready && !moved && !charging && !tripActive && !tripPending;
  if (!climActive) {
    if (idling && powerW > 300.0f && epoch) {
      climActive = true;
      climStartEpoch = epoch;
      climStartMs = now;
      climWh = climWhDay = climWhNight = 0;
      climSocStart = soc;
      climSocEnd = soc;
      Serial.println("[log] climate session started");
    }
  } else {
    if (soc >= 0) climSocEnd = soc;
    if (wh > 0) {
      climWh += wh;
      if (isNight(epoch)) climWhNight += wh; else climWhDay += wh;
    }
    if (!idling) finishClimate("stopped");
  }

  // ---- charges ----
  if (!chgActive) {
    if (charging && epoch) {
      chgActive = true;
      chgStartEpoch = epoch;
      chgStartMs = now;
      chgSocStart = soc;
      chgSocEnd = soc;
      chgKwh = chgKwhDay = chgKwhNight = 0;
    }
  } else {
    if (soc >= 0) chgSocEnd = soc;
    if (wh > 0) {
      chgKwh += wh;
      if (isNight(epoch)) chgKwhNight += wh; else chgKwhDay += wh;
    }
    if (!charging) finishCharge("stopped");
  }

  // Признак активной сессии для авто-записи CAN: машина отвечает по шине и
  // происходит что-то осмысленное (READY, поездка, климат, зарядка).
  sessionActive = (evCanLastFrameAgeSec() < 300UL) &&
                  (ready || tripActive || climActive || chgActive);

  // Точка для графиков (копится на плате, уходит пачкой при связи).
  {
    float kw = (dataOk && v > 0) ? (v * i) / 1000.0f : 0.0f;
    serverApiHistSample(soc, kw, evData.batteryTemp, 0.0f);
  }

  if (odo > 0) lastOdo = odo;
}

// Закрыть открытую поездку перед сном или перезагрузкой.
void tripLogFinishOpen(const char* reason) {
  if (tripActive) finishTrip(reason);
}

bool tripLogIsDriving() {
  return tripActive;
}

bool tripLogIsCharging() {
  return chgActive;
}

bool tripLogSessionActive() {
  return sessionActive;
}

static void sendJsonLines(const char* path) {
  File f = HISTFS.open(path, FILE_READ);
  if (!f) {
    server.send(200, "application/json", "[]");
    return;
  }
  size_t total = countLines(f);
  size_t skip = total > SEND_MAX_RECORDS ? (total - SEND_MAX_RECORDS) : 0;
  f.seek(0);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  size_t index = 0;
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (index++ < skip) continue;
    line.trim();
    if (!line.length()) continue;
    if (!first) server.sendContent(",");
    server.sendContent(line);
    first = false;
    if ((index & 7) == 0) delay(0);
  }
  server.sendContent("]");
  server.sendContent("");
  f.close();
}

void tripLogSendTrips() { sendJsonLines(TRIPS_PATH); }
void tripLogSendCharges() { sendJsonLines(CHARGES_PATH); }

void tripLogClearTrips() {
  tripActive = false;
  HISTFS.remove(TRIPS_PATH);
  tripsCount = 0;
}
void tripLogClearCharges() {
  chgActive = false;
  HISTFS.remove(CHARGES_PATH);
  chargesCount = 0;
}

void tripLogSetNightWindow(uint8_t fromHour, uint8_t toHour) {
  tripLogSetTariff(tariffDay, tariffNight, fromHour, toHour);
}

void tripLogSetTariff(double dayPrice, double nightPrice, uint8_t fromHour, uint8_t toHour) {
  if (fromHour > 23) fromHour = 0;
  if (toHour > 23) toHour = 7;
  if (dayPrice < 0) dayPrice = 0;
  if (nightPrice < 0) nightPrice = 0;
  // Skip the NVS write when nothing changed. The PWA pushes its tariff to the
  // board on every page open; four NVS keys per open eventually exhaust the
  // partition's free pages and every later config write starts failing
  // silently (settings stopped surviving reboots exactly like that).
  if (dayPrice == (double)tariffDay && nightPrice == (double)tariffNight &&
      fromHour == nightFrom && toHour == nightTo) {
    return;
  }
  nightFrom = fromHour;
  nightTo = toHour;
  tariffDay = (float)dayPrice;
  tariffNight = (float)nightPrice;
  Preferences prefs;
  if (prefs.begin("leaftariff", false)) {
    prefs.putUChar("from", fromHour);
    prefs.putUChar("to", toHour);
    prefs.putFloat("day", tariffDay);
    prefs.putFloat("night", tariffNight);
    prefs.end();
  }
}

uint8_t tripLogNightFrom() { return nightFrom; }
uint8_t tripLogNightTo() { return nightTo; }
double tripLogTariffDay() { return tariffDay; }
double tripLogTariffNight() { return tariffNight; }

static size_t fileBytes(const char* path) {
  File f = HISTFS.open(path, FILE_READ);
  if (!f) return 0;
  size_t n = f.size();
  f.close();
  return n;
}

size_t tripLogTripsBytes() { return fileBytes(TRIPS_PATH); }
size_t tripLogChargesBytes() { return fileBytes(CHARGES_PATH); }
size_t tripLogTripsCount() { return tripsCount; }
size_t tripLogChargesCount() { return chargesCount; }
