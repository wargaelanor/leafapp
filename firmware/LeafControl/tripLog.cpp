#include "tripLog.h"

extern void serverApiOnRecord(const char* kind, const char* line);
extern void serverApiClearRecords(const char* scope);
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
#include "udsLeaf.h"

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
extern void noteVehicleAwake();
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

// Накопленная длительность сессий в миллисекундах. Считаем её тиками, а не
// millis()-start: иначе перезагрузка/сон посреди сессии съедает уже
// прошедшее время, а после ребута длительность начинается с нуля.
static uint32_t accTripMs = 0;
static uint32_t accChgMs = 0;
static uint32_t accClimMs = 0;

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
// Пробег, посчитанный по скорости: одометр машины шагает целыми километрами,
// и короткая поездка по нему дала бы 0 км.
static float tripKmSpeed = 0;
static float pendingKmSpeed = 0;
static unsigned long pendingLastMs = 0;

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

static size_t replayOffset[3] = {0, 0, 0};
static uint8_t replayKind = 0;
static size_t replayNextOffset = 0;
static bool historyReady = false;

// Состояние открытой поездки в RTC: переживает перезагрузку и сон, чтобы
// запись не пропадала, если плата ушла в ребут посреди поездки.
RTC_DATA_ATTR static uint8_t rtcTripActive = 0;
RTC_DATA_ATTR static uint32_t rtcTripStart = 0;
RTC_DATA_ATTR static float rtcTripOdoStart = 0;
RTC_DATA_ATTR static float rtcTripOdoLast = 0;
RTC_DATA_ATTR static float rtcTripSocStart = -1;
RTC_DATA_ATTR static float rtcTripKwh = 0;
RTC_DATA_ATTR static uint32_t rtcTripMs = 0;

// Открытая зарядка тоже переживает перезагрузку: заряд идёт часами, и терять
// его целиком из-за ресета (OTA, просадка питания) нельзя — иначе вместо
// одного периода получается два обрывка. Климат — по той же схеме.
RTC_DATA_ATTR static uint8_t rtcChgActive = 0;
RTC_DATA_ATTR static uint32_t rtcChgStart = 0;
RTC_DATA_ATTR static uint32_t rtcChgMs = 0;
RTC_DATA_ATTR static float rtcChgWh = 0, rtcChgWhDay = 0, rtcChgWhNight = 0;
RTC_DATA_ATTR static float rtcChgSocStart = -1;

RTC_DATA_ATTR static uint8_t rtcClimActive = 0;
RTC_DATA_ATTR static uint32_t rtcClimStart = 0;
RTC_DATA_ATTR static uint32_t rtcClimMs = 0;
RTC_DATA_ATTR static float rtcClimWh = 0, rtcClimWhDay = 0, rtcClimWhNight = 0;
RTC_DATA_ATTR static float rtcClimSocStart = -1;

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
  if (strcmp(path, TRIPS_PATH) == 0) replayOffset[0] = 0;
  else if (strcmp(path, CHARGES_PATH) == 0) replayOffset[1] = 0;
  else if (strcmp(path, CLIMATE_PATH) == 0) replayOffset[2] = 0;
  Serial.printf("[log] rotated %s (was %u bytes, kept %u lines)\n",
                path, (unsigned)size, (unsigned)kept);
}

static void appendRecord(const char* path, size_t limitBytes, const char* line) {
  File f = HISTFS.open(path, FILE_APPEND);
  if (!f) {
    Serial.printf("[log] cannot open %s\n", path);
    return;
  }
  size_t written = f.println(line);
  f.close();
  if (written < strlen(line) + 1) {
    Serial.printf("[log] short write to %s: %u bytes\n", path, (unsigned)written);
    return;
  }
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
  historyReady = true;
  tripsCount = fileLines(TRIPS_PATH);
  compactCharges();
  chargesCount = fileLines(CHARGES_PATH);
  climateCount = fileLines(CLIMATE_PATH);
  Serial.printf("[log] history on board: %u trips, %u charges\n",
                (unsigned)tripsCount, (unsigned)chargesCount);
  // Replay all persisted records after boot. Server-side deduplication makes
  // this safe even when older entries were missed before a newer upload.

  // Плата перезагрузилась посреди поездки — записываем то, что успели накопить.
  if (rtcTripActive) {
    rtcTripActive = 0;
    uint32_t endEpoch = epochNow();
    float km = (rtcTripOdoLast > 0 && rtcTripOdoStart > 0) ? (rtcTripOdoLast - rtcTripOdoStart) : 0;
    if (km < 0) km = 0;
    if (rtcTripStart > 0 && km >= 0.2f) {
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

  // Продолжаем зарядку, если плата перезагрузилась посреди неё: заряд идёт
  // часами, и обрыв на ресете раньше давал два огрызка вместо одного периода.
  if (rtcChgActive) {
    chgActive = true;
    chgStartEpoch = rtcChgStart;
    accChgMs = rtcChgMs;
    chgKwh = rtcChgWh;
    chgKwhDay = rtcChgWhDay;
    chgKwhNight = rtcChgWhNight;
    chgSocStart = rtcChgSocStart;
    chgSocEnd = -1;
    Serial.printf("[log] continuing charge session (%.0f Wh, %lu s so far)\n",
                  (double)chgKwh, (unsigned long)(accChgMs / 1000UL));
  }
  if (rtcClimActive) {
    climActive = true;
    climStartEpoch = rtcClimStart;
    accClimMs = rtcClimMs;
    climWh = rtcClimWh;
    climWhDay = rtcClimWhDay;
    climWhNight = rtcClimWhNight;
    climSocStart = rtcClimSocStart;
    climSocEnd = -1;
    Serial.printf("[log] continuing climate session (%.0f Wh so far)\n", (double)climWh);
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
  rtcTripMs = accTripMs;
}

// Снимок открытых зарядки и климата в RTC (вызывается раз в тик).
static void saveSessionsToRtc() {
  rtcChgActive = chgActive ? 1 : 0;
  if (chgActive) {
    rtcChgStart = chgStartEpoch;
    rtcChgMs = accChgMs;
    rtcChgWh = chgKwh;
    rtcChgWhDay = chgKwhDay;
    rtcChgWhNight = chgKwhNight;
    rtcChgSocStart = chgSocStart;
  }
  rtcClimActive = climActive ? 1 : 0;
  if (climActive) {
    rtcClimStart = climStartEpoch;
    rtcClimMs = accClimMs;
    rtcClimWh = climWh;
    rtcClimWhDay = climWhDay;
    rtcClimWhNight = climWhNight;
    rtcClimSocStart = climSocStart;
  }
}

static void finishTrip(const char* reason) {
  if (!tripActive) return;
  tripActive = false;
  saveTripToRtc(false);
  uint32_t endEpoch = epochNow();
  unsigned long durMs = accTripMs;
  float km = (lastOdo > 0 && tripOdoStart > 0) ? (lastOdo - tripOdoStart) : 0;
  if (km < 0) km = 0;
  if (tripKmSpeed > km) km = tripKmSpeed;   // короткие поездки: считаем по скорости
  tripKmSpeed = 0;
  // Раньше здесь проходили «поездки» 0.13 км за 5 секунд: плата только
  // перезагрузилась, EV-CAN ещё не пойман, а UDS уже выдавал скорость.
  // Такие огрызки не поездка — они только дробят историю.
  if (km < 0.2f || durMs < 120000UL) return;

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
  saveSessionsToRtc();
  uint32_t endEpoch = epochNow();
  unsigned long durMs = accChgMs;
  // Отсекаем только совсем короткие всплески тока. Настоящая зарядка — это
  // один период от подключения кабеля до его снятия, даже если ток в середине
  // падал (пауза таймера, добивка на 100 %): именно так её и надо записывать.
  if (durMs < 60000UL || chgKwh < 20.0f) {
    Serial.printf("[log] charge session dropped (%.1f Wh, %lu s)\n",
                  chgKwh, (unsigned long)(durMs / 1000UL));
    return;
  }

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
  Serial.printf("[log] charge %.2f kWh, %lu min, %.0f%% -> %.0f%%\n",
                chgKwh / 1000.0, (unsigned long)(durMs / 60000UL),
                (double)chgSocStart, (double)chgSocEnd);
}

static void finishClimate(const char* reason) {
  if (!climActive) return;
  climActive = false;
  saveSessionsToRtc();
  uint32_t endEpoch = epochNow();
  unsigned long durMs = accClimMs;
  // Климат — это заметная работа: минимум 3 минуты и 30 Вт·ч. Порог ниже
  // прежнего (5 мин / 100 Вт·ч): короткий предпрогрев тоже должен попадать
  // в журнал, иначе он просто исчезает.
  if (durMs < 180000UL || climWh < 30.0f) {
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

// Incremental disk replay. Advance only after server acknowledgement. Reboot
// may resend a record, which is safe because /api/device/session is idempotent.
bool tripLogNextReplay(char* kind, size_t kindSize, char* line, size_t lineSize) {
  if (!historyReady) return false;
  const char* paths[3] = {TRIPS_PATH, CHARGES_PATH, CLIMATE_PATH};
  const char* kinds[3] = {"trip", "charge", "climate"};
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    uint8_t k = (replayKind + attempt) % 3;
    File f = HISTFS.open(paths[k], FILE_READ);
    if (!f) continue;
    if (replayOffset[k] > f.size()) replayOffset[k] = 0; // rotation
    if (replayOffset[k] < f.size() && f.seek(replayOffset[k])) {
      while (f.available()) {
        String record = f.readStringUntil('\n');
        size_t next = f.position();
        record.trim();
        if (record.length() >= 8 && record.length() < lineSize && record.indexOf("\"s\":") >= 0) {
          snprintf(kind, kindSize, "%s", kinds[k]);
          snprintf(line, lineSize, "%s", record.c_str());
          replayKind = k;
          replayNextOffset = next;
          f.close();
          return true;
        }
        replayOffset[k] = next;
      }
    }
    f.close();
  }
  return false;
}

void tripLogReplayConfirmed() {
  replayOffset[replayKind] = replayNextOffset;
  replayKind = (replayKind + 1) % 3;
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

  // Compute motion/selector before charging detection. 0x1DC is a battery
  // acceptance limit, not a cable signal: it is positive during regen too.
  // A charge is only credible when the car is stationary and the pack is
  // being fed (negative current), with either a fresh charger frame or a
  // fresh EV-CAN limit as a hint.
  float powerW = (dataOk && v > 0) ? fabsf(v * i) : 0.0f;
  float powerKw = (dataOk && v > 0) ? (v * i) / 1000.0f : 0.0f;
  bool ready = (evData.vehicleOn == 1);

  float speedKph = evSpeedKph();
  if (speedKph < 0.5f && udsData.speedMs && now - udsData.speedMs < 30000UL &&
      udsData.speed >= 0.0f && udsData.speed <= 250.0f) {
    speedKph = udsData.speed;
  }
  bool speeding = speedKph >= 5.0f;

  bool gearFresh = udsData.gearMs && now - udsData.gearMs < 30000UL;
  bool gearDriving = gearFresh && (udsData.gear == 4 || udsData.gear == 7 ||
                                   udsData.gear == 2);
  bool moving = odoMoving || speeding || gearDriving;
  bool stationary = !moving;

  float chargeLim = evCanChargeLimitAmps();
  bool evCanFresh = evCanLastFrameAgeSec() < 30UL;
  bool evPlugHint = (chargeLim > 0.0f && evCanFresh);
  // isChargingNow comes from the CAR-CAN 0x390 charger frame. It can stay
  // set when the gateway sleeps, so ignore it while the car is moving.
  bool chargerFrame = isChargingNow && stationary;
  bool evChargeRaw = dataOk && v > 100.0f && i < -0.5f && stationary &&
                     (evPlugHint || chargerFrame);
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
  bool evCharging = (evChargeRaw && evSeen >= 3) || evHolding;
  bool charging = chargerFrame || evCharging;
  // Гистерезис: пропадание одного кадра 0x390 (шлюз дремлет) не рвёт сессию.
  // 90 с после последнего признака зарядки — безопасно для 12 В и графиков.
  static uint32_t lastChargingSeenMs = 0;
  if (charging) lastChargingSeenMs = now;
  else if (chgActive && now - lastChargingSeenMs < 90000UL) {
    charging = true;
  }
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

  if (!tripActive && epoch && !charging && speedKph > 0.5f) {
    if (!tripPending || now - pendingLastMs > 30000UL) {
      tripPending = true;
      tripPendEpoch = epoch;
      tripPendMs = now;
      tripPendOdo = lastOdo;
      tripPendSoc = soc;
      pendingKmSpeed = 0;
    } else if (dtMs > 0 && dtMs < 30000UL) {
      pendingKmSpeed += speedKph * ((float)dtMs / 3600000.0f);
    }
    pendingLastMs = now;
  }

  if (!tripActive) {
    // Триггеры: селектор в D/R/Eco, 5 км/ч, 100 м по скорости или одометру.
    if (epoch && !charging && !evChargeRaw &&
        (gearDriving || speeding || pendingKmSpeed >= 0.1f || moved)) {
      tripActive = true;
      // Поездку, начатую селектором, отсчитываем от текущего момента: до этого
      // машина стояла, и «хвост» в минуты нам не нужен.
      bool usePending = !gearDriving && tripPending;
      tripStartEpoch = usePending ? tripPendEpoch : epoch;
      tripStartMs = usePending ? tripPendMs : now;
      tripOdoStart = usePending ? tripPendOdo : lastOdo;
      tripSocStart = usePending ? tripPendSoc : soc;
      tripSocEnd = soc;
      tripKwh = tripKwhDay = tripKwhNight = 0;
      tripKmSpeed = usePending ? pendingKmSpeed : 0;
      tripPending = false;
      pendingKmSpeed = 0;
      tripMoveMs = now;
      tripOffSinceMs = 0;
      accTripMs = 0;
      saveTripToRtc(true);
      Serial.printf("[log] trip started: %.0f km, %.0f km/h, gear=%d\n",
                    (double)tripOdoStart, (double)speedKph, (int)udsData.gear);
    } else if (tripPending && (charging || now - pendingLastMs > 30000UL)) {
      tripPending = false;
      pendingKmSpeed = 0;
    }
  }

  if (tripActive) {
    if (dtMs > 0 && dtMs < 30000UL) accTripMs += dtMs;
    if (dtMs > 0 && dtMs < 30000UL && speedKph > 0.5f) {
      tripKmSpeed += speedKph * ((float)dtMs / 3600000.0f);
    }
    if (moved || speedKph > 0.5f) {
      tripMoveMs = now;
      tripOffSinceMs = 0;
      rtcTripOdoLast = lastOdo;
    }
    if (soc >= 0) tripSocEnd = soc;
    if (wh > 0 && !charging) {
      tripKwh += wh;
      if (isNight(epoch)) tripKwhNight += wh; else tripKwhDay += wh;
      rtcTripKwh = tripKwh;
    }
    // Конец поездки — когда машина выключена: селектор в P, либо нет READY,
    // нет тяги и нет движения. Стоянки в пути (селектор не в P) поездку не
    // делят: раньше её рвало через 3 минуты и получалось «1 км + 16 км».
    bool carOff = (gearFresh && udsData.gear == 1) ||
                  (!ready && powerW < 150.0f &&
                   !(gearFresh && udsData.gear != 1));
    if (!moved && speedKph < 0.5f && !charging && carOff) {
      if (tripOffSinceMs == 0) tripOffSinceMs = now;
    } else {
      tripOffSinceMs = 0;
    }
    // A brief bus gap or a traffic stop is NOT a trip boundary. If all EV
    // traffic vanishes, wait long enough for reconnection before closing.
    // Crucially, "never seen since boot" is NOT a bus loss: after an OTA
    // reboot the CAN sweep takes tens of seconds, and closing here used to
    // produce a pile of 5-second / 0.1-km records at the start of a drive.
    // Also never close while UDS still reports real movement.
    uint32_t canAge = evCanLastFrameAgeSec();
    bool busLost = !charging && speedKph < 1.0f && accTripMs > 600000UL &&
                   (!evCanEverSeen() || canAge > 900UL);
    // Once the car reports off (selector in P, or no READY and no traction),
    // the drive is over. The old 5-minute grace kept counting a parked car.
    // A short confirmation only protects against one dropped CAN frame.
    unsigned long tripGraceMs = 10000UL;
    if ((tripOffSinceMs != 0 && now - tripOffSinceMs > tripGraceMs) || busLost) {
      finishTrip("off");
    } else if (charging && accChgMs > 60000UL) {
      // Машину подключили к зарядке, не «выключив» поездку.
      finishTrip("charge");
    }
  }

  // ---- климат ------------------------------------------------------------
  // Климат считаем по фактическому расходу, а не по флагу READY: на этой
  // машине vehicleOn врёт, и предпрогрев на стоянке из-за него не попадал в
  // журнал вовсе. Условие — машина стоит, не заряжается, идёт заметный ток.
  bool parkedIdle = !moved && speedKph < 0.5f && !charging && !evChargeRaw &&
                    !tripActive;
  static uint8_t climSeen = 0, climStop = 0;
  bool climPower = parkedIdle && powerW > 250.0f;
  if (climPower) {
    if (climSeen < 4) climSeen++;
    climStop = 0;
  } else if (climActive) {
    if (climStop < 6) climStop++;   // 30 с запаса: короткий провал тока не рвёт
  } else {
    climSeen = 0;
  }
  bool climHolding = climActive && climStop < 6 && parkedIdle;
  bool climateOn = (climPower && climSeen >= 4) || climHolding;
  if (!climActive) {
    if (climateOn && epoch) {
      climActive = true;
      climStartEpoch = epoch;
      climStartMs = now;
      accClimMs = 0;
      climWh = climWhDay = climWhNight = 0;
      climSocStart = soc;
      climSocEnd = soc;
      Serial.printf("[log] climate session started (%.0f W)\n", (double)powerW);
    }
  } else {
    if (dtMs > 0 && dtMs < 30000UL) accClimMs += dtMs;
    if (soc >= 0) climSocEnd = soc;
    if (wh > 0) {
      climWh += wh;
      if (isNight(epoch)) climWhNight += wh; else climWhDay += wh;
    }
    if (!climateOn) finishClimate("stopped");
  }

  // ---- зарядки ------------------------------------------------------------
  // Сессия = один период от начала зарядки до снятия кабеля. Внутри ток может
  // пропадать (пауза таймера, добивка на 100 %, короткий сбой шины) — сессию
  // это не рвёт, иначе одна зарядка дробилась на несколько записей.
  static unsigned long chgUnplugMs = 0;
  if (!chgActive) {
    if (charging && epoch) {
      chgActive = true;
      chgStartEpoch = epoch;
      chgStartMs = now;
      accChgMs = 0;
      chgSocStart = soc;
      chgSocEnd = soc;
      chgKwh = chgKwhDay = chgKwhNight = 0;
      chgUnplugMs = 0;
      Serial.printf("[log] charge session started (%.0f%%)\n", (double)soc);
    }
  } else if (moving) {
    // A car cannot charge while moving. This also kills a phantom session
    // started by regeneration during a drive instead of splitting that drive.
    finishCharge("moving");
  } else {
    if (dtMs > 0 && dtMs < 30000UL) accChgMs += dtMs;
    if (soc >= 0) chgSocEnd = soc;
    if (wh > 0) {
      chgKwh += wh;
      if (isNight(epoch)) chgKwhNight += wh; else chgKwhDay += wh;
    }
    // Cable hint: a fresh 0x1DC limit or the charger frame while stationary.
    // 0x1DC alone is never enough - it is positive during regen too.
    if (evPlugHint || chargerFrame) chgUnplugMs = 0;
    else if (chgUnplugMs == 0) chgUnplugMs = now;
    bool unplugged = chgUnplugMs != 0 && now - chgUnplugMs > 120000UL;
    bool busGone = evCanLastFrameAgeSec() > 900UL;
    // Долгая добивка на 100 %: ток почти нулевой, но кабель вставлен — час
    // без тока считаем концом зарядки.
    bool full = !charging && chgSocEnd >= 99.0f &&
                lastChargingSeenMs != 0 && now - lastChargingSeenMs > 3600000UL;
    if (unplugged || busGone) finishCharge("unplugged");
    else if (full) finishCharge("full");
  }

  saveSessionsToRtc();

  // Признак активной сессии для авто-записи CAN: машина отвечает по шине и
  // происходит что-то осмысленное (READY, поездка, климат, зарядка) либо течёт
  // заметная тяга — иначе езда без кадра скорости не попадает в лог вовсе.
  sessionActive = tripActive || chgActive ||
                  ((evCanLastFrameAgeSec() < 300UL) &&
                  (ready || climActive ||
                    powerKw >= 2.0f || powerKw <= -2.0f));
  if (sessionActive) noteVehicleAwake();

  // Точка для графиков (копится на плате, уходит пачкой при связи).
  {
    float kw = (dataOk && v > 0) ? (v * i) / 1000.0f : 0.0f;
    serverApiHistSample(soc, kw, evData.batteryTemp, speedKph);
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

void tripLogClearKind(const char* scope) {
  bool trips = !strcmp(scope, "trips") || !strcmp(scope, "all");
  bool charges = !strcmp(scope, "charges") || !strcmp(scope, "all");
  if (trips) {
    tripActive = false;
    climActive = false;
    saveTripToRtc(false);
    saveSessionsToRtc();
    HISTFS.remove(TRIPS_PATH);
    HISTFS.remove(CLIMATE_PATH);
    tripsCount = climateCount = 0;
    replayOffset[0] = replayOffset[2] = 0;
  }
  if (charges) {
    chgActive = false;
    saveSessionsToRtc();
    HISTFS.remove(CHARGES_PATH);
    chargesCount = 0;
    replayOffset[1] = 0;
  }
  replayKind = 0;
  serverApiClearRecords(scope);
}
void tripLogClearTrips() { tripLogClearKind("trips"); }
void tripLogClearCharges() { tripLogClearKind("charges"); }

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
