#include "tripLog.h"
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

// Board-side history: the phone can be closed, the board keeps counting. One
// JSON line per finished trip / charge session in the FAT partition.
//
// Sizing: a record is ~110-150 bytes, so 1 MB holds roughly 6000 trips and
// 256 KB holds roughly 1600 charge sessions. When a file grows past its limit
// the older half is dropped (see rotateFile()).

extern EvBatteryData evData;
extern bool isChargingNow;
extern WebServer server;

static const char* TRIPS_PATH   = "/trips.jsonl";
static const char* CHARGES_PATH = "/charges.jsonl";
static const size_t TRIPS_LIMIT   = BOARD_TRIPS_LIMIT;
static const size_t CHARGES_LIMIT = BOARD_CHARGES_LIMIT;
static const size_t SEND_MAX_RECORDS = 800;

// Moscow, no DST. The board stores UTC epochs; localtime_r() on this core does
// not always apply the configTime() offset, so the hour is calculated directly.
static const uint32_t TZ_OFFSET_SEC = 3UL * 3600UL;
static uint8_t nightFrom = 0;   // 00:00
static uint8_t nightTo = 7;     // 07:00

static unsigned long lastTickMs = 0;
static float lastOdo = -1.0f;

static size_t tripsCount = 0;
static size_t chargesCount = 0;

// --- trip state ----------------------------------------------------------
static bool tripActive = false;
static uint32_t tripStartEpoch = 0;
static unsigned long tripStartMs = 0;
static float tripOdoStart = 0;
static float tripSocStart = -1;
static float tripSocEnd = -1;
static float tripKwh = 0, tripKwhDay = 0, tripKwhNight = 0;
static unsigned long tripLastMoveMs = 0;

// --- charge state --------------------------------------------------------
static bool chgActive = false;
static uint32_t chgStartEpoch = 0;
static unsigned long chgStartMs = 0;
static float chgSocStart = -1;
static float chgSocEnd = -1;
static float chgKwh = 0, chgKwhDay = 0, chgKwhNight = 0;

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

void tripLogInit() {
  // T-Call has a small SPIFFS partition and may need the initial format;
  // the T-2CAN FAT partition must never be formatted here because canLogger
  // shares it.
  Preferences prefs;
  if (prefs.begin("leaftariff", false)) {
    nightFrom = prefs.getUChar("from", 0);
    nightTo = prefs.getUChar("to", 7);
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
  chargesCount = fileLines(CHARGES_PATH);
  Serial.printf("[log] history on board: %u trips, %u charges\n",
                (unsigned)tripsCount, (unsigned)chargesCount);
}

static void finishTrip(const char* reason) {
  if (!tripActive) return;
  tripActive = false;
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
  Serial.printf("[log] trip %.2f km, %.2f kWh\n", km, tripKwh);
}

static void finishCharge(const char* reason) {
  if (!chgActive) return;
  chgActive = false;
  uint32_t endEpoch = epochNow();
  unsigned long durMs = millis() - chgStartMs;
  if (durMs < 60000UL && chgKwh < 0.02f) return;

  char line[200];
  // "w"/"wd"/"wn" are watt-hours (Wh), not kWh. The PWA stores Wh directly.
  snprintf(line, sizeof(line),
           "{\"s\":%lu,\"e\":%lu,\"d\":%lu,\"w\":%.3f,\"wd\":%.3f,\"wn\":%.3f,\"s0\":%d,\"s1\":%d,\"r\":\"%s\"}",
           (unsigned long)chgStartEpoch, (unsigned long)endEpoch,
           (unsigned long)(durMs / 1000UL), chgKwh, chgKwhDay, chgKwhNight,
           (int)(chgSocStart >= 0 ? chgSocStart : -1),
           (int)(chgSocEnd >= 0 ? chgSocEnd : -1), reason);
  appendRecord(CHARGES_PATH, CHARGES_LIMIT, line);
  Serial.printf("[log] charge %.2f kWh\n", chgKwh);
}

void tripLogTick() {
  unsigned long now = millis();
  if (now - lastTickMs < 5000UL) return;
  unsigned long dtMs = lastTickMs ? (now - lastTickMs) : 0;
  lastTickMs = now;

  float odo = (float)evData.odometerKm;
  float soc = evData.instrumentSOC >= 0 ? evData.instrumentSOC : -1;
  float v = evData.packVoltage;
  float i = evData.packCurrent;
  bool dataOk = evData.packDataValid && evData.packVoltageValid;
  bool charging = isChargingNow;
  uint32_t epoch = epochNow();

  // Energy of the interval, split by local time for the day/night tariff.
  float wh = 0;
  if (dtMs > 0 && dtMs < 30000UL && dataOk && v > 0) {
    wh = fabsf(v * i) * (dtMs / 3600000.0f);
  }

  // Movement is the only honest trip trigger: vehicleOn on this car often
  // stays 0/-1, and relying on it made the board skip whole drives.
  bool moved = (odo > 0 && lastOdo > 0 && odo > lastOdo + 0.02f);

  // ---- trips ----
  if (!tripActive) {
    if (moved && epoch) {
      tripActive = true;
      tripStartEpoch = epoch;
      tripStartMs = now;
      tripOdoStart = lastOdo;
      tripSocStart = soc;
      tripSocEnd = soc;
      tripKwh = tripKwhDay = tripKwhNight = 0;
      tripLastMoveMs = now;
      Serial.printf("[log] trip started at %.1f km\n", (double)tripOdoStart);
    }
  } else {
    if (moved) tripLastMoveMs = now;
    if (soc >= 0) tripSocEnd = soc;
    if (wh > 0) {
      tripKwh += wh;
      if (isNight(epoch)) tripKwhNight += wh; else tripKwhDay += wh;
    }
    // Ignition off plus a short standstill closes a trip; a long standstill
    // closes it even when the car still reports "on" (e.g. waiting in traffic
    // was long but the odometer never moved - do not stretch that to a day).
    bool off = (evData.vehicleOn == 0) && (now - tripLastMoveMs > 180000UL);
    bool idle = (now - tripLastMoveMs > 15UL * 60000UL);
    if (off) finishTrip("off");
    else if (idle) finishTrip("idle");
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

  if (odo > 0) lastOdo = odo;
}

bool tripLogIsDriving() {
  return tripActive;
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
  if (fromHour > 23) fromHour = 0;
  if (toHour > 23) toHour = 7;
  nightFrom = fromHour;
  nightTo = toHour;
  Preferences prefs;
  if (prefs.begin("leaftariff", false)) {
    prefs.putUChar("from", fromHour);
    prefs.putUChar("to", toHour);
    prefs.end();
  }
}

uint8_t tripLogNightFrom() { return nightFrom; }
uint8_t tripLogNightTo() { return nightTo; }

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
