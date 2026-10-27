#include "canLogger.h"
#include <WebServer.h>
#include <FFat.h>

extern WebServer server;

struct __attribute__((packed)) CanLogRecord {
  uint32_t timestampMs;
  uint32_t id;
  uint8_t data[8];
  uint8_t len;
  char bus;
  char direction;
};

static File logFile;
static CanLogRecord pending[256];
static size_t pendingCount = 0;
static size_t recordCount = 0;
static size_t recordCapacity = 0;
static uint32_t recordingStartedMs = 0;
static bool recording = false;
static bool full = false;
static bool fsReady = false;

static bool mountFs() {
  if (fsReady) return true;
  fsReady = FFat.begin(true);
  if (fsReady) recordCapacity = (FFat.totalBytes() - 4096U) / sizeof(CanLogRecord);
  return fsReady;
}

static bool flushPending() {
  if (!logFile || pendingCount == 0) return true;
  size_t bytes = pendingCount * sizeof(CanLogRecord);
  bool ok = logFile.write(reinterpret_cast<const uint8_t*>(pending), bytes) == bytes;
  if (ok) logFile.flush();
  pendingCount = 0;
  return ok;
}

bool canLoggerStart() {
  if (!mountFs()) return false;
  if (logFile) logFile.close();
  logFile = FFat.open("/canlog.bin", FILE_WRITE);
  if (!logFile) return false;
  pendingCount = 0;
  recordCount = 0;
  full = false;
  recordingStartedMs = millis();
  recording = true;
  return true;
}

void canLoggerStop() {
  recording = false;
  if (!flushPending()) full = true;
  if (logFile) logFile.close();
}

void canLoggerClear() {
  canLoggerStop();
  if (mountFs()) FFat.remove("/canlog.bin");
  recordCount = 0;
  full = false;
}

void canLoggerAppend(char bus, char direction, uint32_t id, const uint8_t* data, uint8_t len) {
  if (!recording || !logFile) return;
  if (recordCount >= recordCapacity) {
    full = true;
    canLoggerStop();
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
  if (pendingCount == sizeof(pending) / sizeof(pending[0]) && !flushPending()) {
    full = true;
    canLoggerStop();
  }
}

bool canLoggerIsRecording() { return recording; }
bool canLoggerIsFull() { return full; }
size_t canLoggerCount() { return recordCount; }
size_t canLoggerCapacity() {
  mountFs();
  return recordCapacity;
}

void canLoggerSendDownload() {
  if (recording) canLoggerStop();
  if (!mountFs()) {
    server.send(500, "text/plain", "CAN log storage unavailable");
    return;
  }
  File input = FFat.open("/canlog.bin", FILE_READ);
  if (!input) {
    server.send(404, "text/plain", "CAN log is empty");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=leaf-can-log.txt");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");
  server.sendContent("# Leaf Control CAN log\r\n# time_s bus dir id len data\r\n");

  char line[96];
  String chunk;
  chunk.reserve(1024);
  CanLogRecord record;
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
      server.sendContent(chunk);
      chunk = "";
      delay(0);
    }
  }
  input.close();
  if (chunk.length()) server.sendContent(chunk);
  if (full) server.sendContent("# BUFFER FULL - recording stopped automatically\r\n");
  server.sendContent("");
}
