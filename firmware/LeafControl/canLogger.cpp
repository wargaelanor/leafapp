#include "canLogger.h"
#include <WebServer.h>
#include <FFat.h>

extern WebServer server;

// Формат записи: 19 байт, little-endian (совпадает с разбором на сервере).
struct __attribute__((packed)) CanLogRecord {
  uint32_t timestampMs;
  uint32_t id;
  uint8_t data[8];
  uint8_t len;
  char bus;
  char direction;
};

static const char* MANUAL_PATH = "/canlog.bin";
static const char* AUTO_PREFIX = "can_";      // /can_<epoch>.bin
static const uint32_t AUTO_TAIL_MS = 300000UL;   // 5 минут после конца сессии
static const uint32_t AUTO_MAX_MS = 5400000UL;   // страховка: 90 минут
static const size_t AUTO_MAX_BYTES = 7UL * 1024UL * 1024UL;
static const size_t AUTO_RESERVE = 512UL * 1024UL;  // оставить под историю
static const size_t AUTO_KEEP = 3;               // сколько сессий ждут выгрузки
static const uint32_t TICK_MS = 1000UL;

static File logFile;
static CanLogRecord pending[256];
static size_t pendingCount = 0;
static size_t recordCount = 0;
static size_t capacityRecords = 0;
static uint32_t recordingStartedMs = 0;
static bool recording = false;
static bool full = false;
static bool fsReady = false;

static bool autoSession = false;
static bool autoHeld = false;
static unsigned long autoTailEndMs = 0;
static unsigned long autoTickMs = 0;
static size_t sessionLimit = AUTO_MAX_BYTES;
static size_t sessionBytes = 0;
static char sessionPath[40] = "/canlog.bin";

static bool mountFs() {
  if (fsReady) return true;
  // Никогда не форматируем раздел: на нём же лежит история поездок.
  fsReady = FFat.begin(false);
  if (fsReady) capacityRecords = (FFat.totalBytes() - 4096U) / sizeof(CanLogRecord);
  return fsReady;
}

static size_t freeBytes() {
  if (!mountFs()) return 0;
  size_t total = FFat.totalBytes();
  size_t used = FFat.usedBytes();
  return total > used ? total - used : 0;
}

static const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool isSessionName(const char* name) {
  const char* b = baseName(name);
  return strncmp(b, AUTO_PREFIX, strlen(AUTO_PREFIX)) == 0 &&
         strstr(b, ".bin") != nullptr && b[0] != '\0';
}

static bool isCurrentFile(const char* name) {
  return recording && !strcmp(baseName(name), baseName(sessionPath));
}

// Сессия из очереди: newest=true — самая свежая, иначе самая старая.
static bool findSession(bool newest, char* out, size_t outSize, size_t* bytes) {
  if (!mountFs()) return false;
  File dir = FFat.open("/");
  if (!dir) return false;
  char best[40] = {0};
  size_t bestSize = 0;
  File f;
  while ((f = dir.openNextFile())) {
    const char* nm = f.name();
    if (isSessionName(nm) && !isCurrentFile(nm)) {
      const char* b = baseName(nm);
      bool better = !best[0] || (newest ? (strcmp(b, best) > 0) : (strcmp(b, best) < 0));
      if (better) {
        snprintf(best, sizeof(best), "%s", b);
        bestSize = f.size();
      }
    }
    f.close();
  }
  dir.close();
  if (!best[0]) return false;
  if (out) snprintf(out, outSize, "%s", best);
  if (bytes) *bytes = bestSize;
  return true;
}

static size_t countSessions() {
  if (!mountFs()) return 0;
  File dir = FFat.open("/");
  if (!dir) return 0;
  size_t n = 0;
  File f;
  while ((f = dir.openNextFile())) {
    if (isSessionName(f.name()) && !isCurrentFile(f.name())) n++;
    f.close();
  }
  dir.close();
  return n;
}

static void evictOldSessions() {
  char name[40];
  size_t bytes = 0;
  // Место кончается — жертвуем самыми свежими кусками: начало поездки
  // (запуск машины) ценнее для разбора, чем её середина.
  while (countSessions() >= AUTO_KEEP && findSession(true, name, sizeof(name), &bytes)) {
    char path[48];
    snprintf(path, sizeof(path), "/%s", name);
    Serial.printf("[can] мало места, удаляю сессию %s (%u КБ)\n",
                  name, (unsigned)(bytes / 1024));
    FFat.remove(path);
  }
}

static bool flushPending() {
  if (!logFile || pendingCount == 0) return true;
  size_t bytes = pendingCount * sizeof(CanLogRecord);
  bool ok = logFile.write(reinterpret_cast<const uint8_t*>(pending), bytes) == bytes;
  if (ok) logFile.flush();
  pendingCount = 0;
  return ok;
}

static bool startSession(const char* path, size_t limit) {
  if (!mountFs()) return false;
  if (logFile) logFile.close();
  logFile = FFat.open(path, FILE_WRITE);
  if (!logFile) return false;
  snprintf(sessionPath, sizeof(sessionPath), "%s", path);
  pendingCount = 0;
  recordCount = 0;
  full = false;
  sessionBytes = 0;
  sessionLimit = limit;
  recordingStartedMs = millis();
  recording = true;
  return true;
}

static void closeSession() {
  recording = false;
  if (!flushPending()) full = true;
  if (logFile) logFile.close();
}

// ---------------------------------------------------------------- ручной режим

bool canLoggerStart() {
  if (recording) return true;
  size_t free = freeBytes();
  if (free < 256UL * 1024UL) return false;
  autoSession = false;
  if (!startSession(MANUAL_PATH, free - 4096U)) return false;
  Serial.println("[can] ручная запись начата");
  return true;
}

void canLoggerStop() {
  if (!recording) return;
  bool wasAuto = autoSession;
  autoSession = false;
  closeSession();
  Serial.printf("[can] запись остановлена: %u кадров\n", (unsigned)recordCount);
  if (wasAuto && recordCount == 0) FFat.remove(sessionPath);
}

void canLoggerClear() {
  canLoggerStop();
  autoSession = false;
  if (mountFs()) FFat.remove(MANUAL_PATH);
  recordCount = 0;
  full = false;
}

void canLoggerAppend(char bus, char direction, uint32_t id, const uint8_t* data, uint8_t len) {
  if (!recording || !logFile) return;
  if (recordCount >= capacityRecords ||
      sessionBytes + sizeof(CanLogRecord) > sessionLimit) {
    full = true;
    if (autoSession) {
      Serial.printf("[can] сессия заполнена (%u КБ)\n", (unsigned)(sessionBytes / 1024));
      closeSession();
      autoSession = false;
    } else {
      canLoggerStop();
    }
    return;
  }
  CanLogRecord& record = pending[pendingCount++];
  record.timestampMs = millis() - recordingStartedMs;
  record.id = id;
  record.len = len > 8 ? 8 : len;
  record.bus = bus;
  record.direction = direction;
  memset(record.data, 0, sizeof(record.data));
  if (data && record.len) memcpy(record.data, data, record.len);
  recordCount++;
  sessionBytes += sizeof(CanLogRecord);
  if (pendingCount == sizeof(pending) / sizeof(pending[0]) && !flushPending()) {
    full = true;
    if (autoSession) {
      closeSession();
      autoSession = false;
    } else {
      canLoggerStop();
    }
  }
}

bool canLoggerIsRecording() { return recording; }
bool canLoggerIsFull() { return full; }
bool canLoggerIsAuto() { return autoSession; }
size_t canLoggerSessionBytes() { return sessionBytes; }
bool canLoggerHasLog() {
  return mountFs() && FFat.exists(MANUAL_PATH);
}
size_t canLoggerCount() { return recordCount; }
size_t canLoggerCapacity() {
  mountFs();
  return capacityRecords;
}
size_t canLoggerPendingCount() { return countSessions(); }

// ---------------------------------------------------------------- авто-сессии

static void startAutoSession() {
  static unsigned long retryAt = 0;
  unsigned long now = millis();
  if (retryAt != 0 && (long)(now - retryAt) < 0) return;
  evictOldSessions();
  size_t free = freeBytes();
  if (free < AUTO_RESERVE + 256UL * 1024UL) {
    Serial.printf("[can] нет места для записи (свободно %u КБ)\n", (unsigned)(free / 1024));
    retryAt = now + 60000UL;   // не долбим файловую систему каждую секунду
    return;
  }
  size_t limit = AUTO_MAX_BYTES;
  if (limit > free - AUTO_RESERVE) limit = free - AUTO_RESERVE;

  time_t t = time(nullptr);
  uint32_t epoch = (t > 1600000000) ? (uint32_t)t : (uint32_t)(millis() / 1000UL);
  char path[40];
  snprintf(path, sizeof(path), "/can_%lu.bin", (unsigned long)epoch);
  autoSession = true;
  if (!startSession(path, limit)) {
    autoSession = false;
    Serial.println("[can] не удалось открыть файл сессии");
    return;
  }
  Serial.printf("[can] сессия %s начата (лимит %u КБ)\n",
                path, (unsigned)(limit / 1024));
}

static void finishAutoSession(const char* reason) {
  if (!recording) return;
  closeSession();
  if (recordCount == 0) {
    FFat.remove(sessionPath);
    Serial.printf("[can] сессия пустая, файл удалён (%s)\n", reason);
  } else {
    Serial.printf("[can] сессия %s завершена: %u кадров, %u КБ (%s)\n",
                  sessionPath, (unsigned)recordCount,
                  (unsigned)(sessionBytes / 1024), reason);
  }
  autoSession = false;
  sessionBytes = 0;
}

void canLoggerAutoTick(bool active) {
  unsigned long now = millis();
  if (autoTickMs != 0 && now - autoTickMs < TICK_MS) return;
  autoTickMs = now;

  // Ручную запись с веб-страницы авторежим не трогает.
  if (recording && !autoSession) return;

  if (active) {
    autoHeld = true;
    autoTailEndMs = 0;
    if (!recording) startAutoSession();
  } else {
    if (autoHeld) {
      autoHeld = false;
      autoTailEndMs = now + AUTO_TAIL_MS;
      Serial.println("[can] сессия закончилась, пишу ещё 5 минут");
    }
    if (recording && autoTailEndMs != 0 && (long)(now - autoTailEndMs) >= 0) {
      finishAutoSession("tail");
      autoTailEndMs = 0;
    }
  }

  if (recording && autoSession) {
    if (now - recordingStartedMs > AUTO_MAX_MS) finishAutoSession("time");
  }
}

// ---------------------------------------------------------------- выгрузка

bool canLoggerNextPending(char* out, size_t outSize, size_t* bytes) {
  return findSession(false, out, outSize, bytes);
}

bool canLoggerExists(const char* name) {
  if (!mountFs() || !name) return false;
  char path[48];
  snprintf(path, sizeof(path), "/%s", baseName(name));
  return FFat.exists(path);
}

size_t canLoggerReadAt(const char* name, size_t off, uint8_t* buf, size_t maxLen) {
  if (!mountFs() || !name || !buf || !maxLen) return 0;
  char path[48];
  snprintf(path, sizeof(path), "/%s", baseName(name));
  File f = FFat.open(path, FILE_READ);
  if (!f) return 0;
  if (!f.seek(off)) { f.close(); return 0; }
  size_t n = f.read(buf, maxLen);
  f.close();
  return n;
}

void canLoggerMarkSent(const char* name) {
  if (!mountFs() || !name) return;
  char path[48];
  snprintf(path, sizeof(path), "/%s", baseName(name));
  FFat.remove(path);
  Serial.printf("[can] файл %s удалён после выгрузки\n", path);
}

// ---------------------------------------------------------------- страница

void canLoggerSendDownload() {
  if (recording) canLoggerStop();
  if (!mountFs()) {
    server.send(500, "text/plain", "CAN log storage unavailable");
    return;
  }
  // Если ручной записи нет, отдаём самую свежую авто-сессию.
  char path[48];
  snprintf(path, sizeof(path), "%s", MANUAL_PATH);
  if (!FFat.exists(path)) {
    char name[40] = {0};
    // самая новая = последняя по имени, поэтому перебираем и берём максимум
    File dir = FFat.open("/");
    if (dir) {
      char best[40] = {0};
      File f;
      while ((f = dir.openNextFile())) {
        if (isSessionName(f.name())) {
          const char* b = baseName(f.name());
          if (!best[0] || strcmp(b, best) > 0) snprintf(best, sizeof(best), "%s", b);
        }
        f.close();
      }
      dir.close();
      if (best[0]) snprintf(name, sizeof(name), "%s", best);
    }
    if (!name[0]) {
      server.send(404, "text/plain", "CAN log is empty");
      return;
    }
    snprintf(path, sizeof(path), "/%s", name);
  }

  File input = FFat.open(path, FILE_READ);
  if (!input) {
    server.send(404, "text/plain", "CAN log is empty");
    return;
  }
  static const char header[] =
      "# Leaf Control CAN log\r\n# time_s bus dir id len data\r\n";
  char line[96];
  CanLogRecord record;

  // Точная длина заранее: chunked-ответы на слабом Wi-Fi обрывались пустым
  // чанком и подвешивали loop() на всю загрузку.
  size_t textLength = sizeof(header) - 1;
  while (input.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
    int used = snprintf(line, sizeof(line), "%lu.%03lu %c %c 0x%03lX %u",
                        (unsigned long)(record.timestampMs / 1000UL),
                        (unsigned long)(record.timestampMs % 1000UL),
                        record.bus, record.direction, (unsigned long)record.id, record.len);
    for (uint8_t d = 0; d < record.len && used > 0 && used < (int)sizeof(line) - 6; d++) {
      used += snprintf(line + used, sizeof(line) - used, " %02X", record.data[d]);
    }
    textLength += used + 2; // CRLF
  }
  if (full) textLength += strlen("# BUFFER FULL - recording stopped automatically\r\n");
  input.seek(0);

  NetworkClient& client = server.client();
  client.setTimeout(750);
  server.sendHeader("Content-Disposition", "attachment; filename=leaf-can-log.txt");
  server.setContentLength(textLength);
  server.send(200, "text/plain; charset=utf-8", "");
  if (!client.connected() || client.write((const uint8_t*)header, sizeof(header) - 1) != sizeof(header) - 1) {
    input.close();
    client.stop();
    return;
  }

  String chunk;
  chunk.reserve(1024);
  while (input.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
    int used = snprintf(line, sizeof(line), "%lu.%03lu %c %c 0x%03lX %u",
                        (unsigned long)(record.timestampMs / 1000UL),
                        (unsigned long)(record.timestampMs % 1000UL),
                        record.bus, record.direction, (unsigned long)record.id, record.len);
    for (uint8_t d = 0; d < record.len && used > 0 && used < (int)sizeof(line) - 6; d++) {
      used += snprintf(line + used, sizeof(line) - used, " %02X", record.data[d]);
    }
    snprintf(line + used, sizeof(line) - used, "\r\n");
    chunk += line;
    if (chunk.length() >= 900) {
      if (!client.connected() || client.write((const uint8_t*)chunk.c_str(), chunk.length()) != chunk.length()) {
        input.close();
        client.stop();
        return;
      }
      chunk = "";
      delay(0);
    }
  }
  input.close();
  if (chunk.length() && client.connected()) {
    client.write((const uint8_t*)chunk.c_str(), chunk.length());
  }
  if (full && client.connected()) {
    static const char fullMessage[] = "# BUFFER FULL - recording stopped automatically\r\n";
    client.write((const uint8_t*)fullMessage, sizeof(fullMessage) - 1);
  }
  client.stop();
}
