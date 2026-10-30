#include "evCanLeaf.h"
#include <SPI.h>
#include <Preferences.h>
#include "mcp2515.h"
#include "canLogger.h"

// T-2CAN Port A pins (LilyGO pin_config.h)
#define EVCAN_SPI_SCLK 12
#define EVCAN_SPI_MISO 13
#define EVCAN_SPI_MOSI 11
#define EVCAN_SPI_CS   10
#define EVCAN_MCP_RST  9

static MCP2515 evCan(EVCAN_SPI_CS, 10000000, &SPI);
static bool evCanOk = false;
uint32_t canFramesA = 0;

// ---- Timing candidates -------------------------------------------------
// The board should carry a 16 MHz crystal, but clones and other revisions
// exist. Instead of trusting silkscreen or a datasheet, listen at every
// plausible clock/bitrate and keep the combination that actually decodes
// valid frames. Port A stays in listen-only mode for every candidate, so
// nothing is ever transmitted or acknowledged on the battery bus.
struct EvCanTiming {
  CAN_SPEED speed;
  CAN_CLOCK clock;
  const char* speedName;
  const char* clockName;
  uint8_t cnf1;
  uint8_t cnf2;
  uint8_t cnf3;
};

static const EvCanTiming kTimings[] = {
  {CAN_500KBPS, MCP_16MHZ, "500k", "16MHz",
   MCP_16MHz_500kBPS_CFG1, MCP_16MHz_500kBPS_CFG2, MCP_16MHz_500kBPS_CFG3},
  {CAN_500KBPS, MCP_8MHZ,  "500k", "8MHz",
   MCP_8MHz_500kBPS_CFG1, MCP_8MHz_500kBPS_CFG2, MCP_8MHz_500kBPS_CFG3},
  {CAN_500KBPS, MCP_20MHZ, "500k", "20MHz",
   MCP_20MHz_500kBPS_CFG1, MCP_20MHz_500kBPS_CFG2, MCP_20MHz_500kBPS_CFG3},
  {CAN_250KBPS, MCP_16MHZ, "250k", "16MHz",
   MCP_16MHz_250kBPS_CFG1, MCP_16MHz_250kBPS_CFG2, MCP_16MHz_250kBPS_CFG3},
  {CAN_250KBPS, MCP_8MHZ,  "250k", "8MHz",
   MCP_8MHz_250kBPS_CFG1, MCP_8MHz_250kBPS_CFG2, MCP_8MHz_250kBPS_CFG3},
  {CAN_250KBPS, MCP_20MHZ, "250k", "20MHz",
   MCP_20MHz_250kBPS_CFG1, MCP_20MHz_250kBPS_CFG2, MCP_20MHz_250kBPS_CFG3},
  {CAN_125KBPS, MCP_16MHZ, "125k", "16MHz",
   MCP_16MHz_125kBPS_CFG1, MCP_16MHz_125kBPS_CFG2, MCP_16MHz_125kBPS_CFG3},
  {CAN_125KBPS, MCP_8MHZ,  "125k", "8MHz",
   MCP_8MHz_125kBPS_CFG1, MCP_8MHz_125kBPS_CFG2, MCP_8MHz_125kBPS_CFG3},
  {CAN_125KBPS, MCP_20MHZ, "125k", "20MHz",
   MCP_20MHz_125kBPS_CFG1, MCP_20MHz_125kBPS_CFG2, MCP_20MHz_125kBPS_CFG3},
};
static const uint8_t kTimingCount = sizeof(kTimings) / sizeof(kTimings[0]);

static const uint32_t kScanWindowFastMs = 500;   // per candidate while looking
static const uint32_t kScanWindowSlowMs = 2000;  // after a few empty passes
static const uint32_t kScanSlowAfterMs  = 120000UL;
static const uint32_t kLockFrames       = 25;    // frames needed to accept a timing
static const uint32_t kAutoScanAfterMs  = 20000;
static const uint32_t kRescanIntervalMs = 300000UL;
static const uint32_t kStaleLockSeconds = 60;    // locked but silent -> re-scan

static uint8_t  activeTiming = 0;
static bool     timingLocked = false;
static bool     scanActive = false;
static bool     scanCompleted = false;
static uint8_t  scannedInPass = 0;
static uint32_t scanPasses = 0;
static uint32_t lastScanEndMs = 0;
static uint8_t  scanCursor = 0;
static uint32_t scanWindowStart = 0;
static uint32_t scanWindowBaseline = 0;
static uint32_t scanStartedAt = 0;
static uint8_t  scanBestIndex = 0xFF;
static uint32_t scanBestFrames = 0;
static uint8_t  scanResults[kTimingCount];
static uint8_t  lastRxError = 0;
static uint8_t  lastTxError = 0;
static uint8_t  lastErrorFlags = 0;
static bool     chipLooksValid = false;
static bool     lastCfgWriteOk = true;
static uint8_t  emptySweeps = 0;
static uint32_t evHardResets = 0;
static uint32_t lastEvFrameMs = 0;
static bool     evFrameSeen = false;
static uint32_t lastErrRefreshMs = 0;
// Raw payloads of the frames that carry battery data, for remote decoding.
static uint8_t  rawPack1db[8];
static uint8_t  rawPack1dbLen = 0;
static uint8_t  rawTemp5c0[8];
static uint8_t  rawTemp5c0Len = 0;

static void handleEvFrame(uint32_t id, uint8_t len, const uint8_t* data) {
  if (id == 0x5BC && len >= 5) {
    // LB_Remain_Capacity_GIDS: start bit 7, len 10, motorola -> byte0<<2 | byte1>>6
    int gids = (data[0] << 2) | (data[1] >> 6);
    // LB_Capacity_Deterioration_Rate (SOH): start bit 33, len 7, intel -> byte4>>1
    int soh = (data[4] >> 1) & 0x7F;
    // IT-CAN and EV-CAN both carry a 0x7D00 service variant that decodes to a
    // fake 500 GIDS. Port B already filters it; Port A needs the same rule.
    bool gidsFrame = !(data[0] == 0x7D && (data[1] & 0xC0) == 0x00);
    if (gidsFrame) handleEvGids(gids);
    handleEvSOH(soh);
  } else if (id == 0x1DB && len >= 4) {
    rawPack1dbLen = len > 8 ? 8 : len;
    memcpy(rawPack1db, data, rawPack1dbLen);
    // LB_Total_Voltage: start bit 23, len 10, 0.5V/bit -> byte2<<2 | byte3>>6
    int voltRaw = (data[2] << 2) | (data[3] >> 6);
    float voltage = voltRaw * 0.5f;
    // LB_Current: start bit 7, len 11 signed, 0.5A/bit -> byte0<<3 | byte1>>5
    int curRaw = (data[0] << 3) | (data[1] >> 5);
    if (curRaw & 0x400) curRaw -= 0x800; // sign-extend 11 bits
    // OVMS convention: battery output/discharge is positive, charge/regen negative.
    float current = curRaw * -0.5f;
    handleEvPackStatus(voltage, current);
  } else if (id == 0x54C && len >= 7) {
    // OutsideAmbientTemperature: byte6, 0.5C/bit, offset -40 (OVMS xnl)
    float ambient = data[6] * 0.5f - 40.0f;
    if (data[6] != 0xFF) handleEvAmbient(ambient);
  } else if (id == 0x5C0 && len >= 3) {
    rawTemp5c0Len = len > 8 ? 8 : len;
    memcpy(rawTemp5c0, data, rawTemp5c0Len);
    // LBC pack temperature, as used by OVMS. Valid when the multiplex bits are 01.
    if ((data[0] >> 6) == 1) {
      float temp = data[2] * 0.5f - 40.0f;
      if (temp >= -40.0f && temp <= 80.0f) handleBatteryTemp(temp);
    }
  } else if (id == 0x59E && len >= 8) {
    // ZE1 dashboard SOC: byte7 * 0.5 (OVMS patch Apr 2026, samr037).
    // Examples: 0x6E -> 55 %, 0xC8 -> 100 %, 0xAC -> 86 %
    float soc = data[7] * 0.5f;
    if (soc > 0.0f && soc <= 100.0f) handleEvInstrumentSOC(soc);
  } else if (id == 0x11A && len >= 2) {
    // CarOnOffStatus: 4-bit field at bits12-15 (intel), byte1 bits4-7.
    // The old 3-bit extraction could never produce the compared value 8,
    // so the "on" branch was dead code. Values 8=on / 4=off — verify on car.
    int status = (data[1] >> 4) & 0x0F;
    if (status == 8) handleEvVehicleOn(true);
    else if (status == 4) handleEvVehicleOn(false);
  }
}

// Direct MCP2515 register read, used only for read-only diagnostics.
static uint8_t mcpReadRegister(uint8_t address) {
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  digitalWrite(EVCAN_SPI_CS, LOW);
  SPI.transfer(0x03);        // READ instruction
  SPI.transfer(address);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(EVCAN_SPI_CS, HIGH);
  SPI.endTransaction();
  return value;
}

static void storeTiming(uint8_t idx) {
  Preferences prefs;
  if (!prefs.begin("evcan", false)) return;
  prefs.putUChar("idx", idx);
  prefs.end();
}

// Configure the controller for one candidate. Returns false when the
// MCP2515 rejects the request or the write did not reach the CNF registers.
static bool applyTiming(uint8_t idx) {
  const EvCanTiming& t = kTimings[idx];
  if (evCan.setBitrate(t.speed, t.clock) != MCP2515::ERROR_OK) return false;
  if (evCan.setListenOnlyMode() != MCP2515::ERROR_OK) return false;
  uint8_t cnf1 = mcpReadRegister(0x2A);
  uint8_t cnf2 = mcpReadRegister(0x29);
  uint8_t cnf3 = mcpReadRegister(0x28);
  lastCfgWriteOk = (cnf1 == t.cnf1 && cnf2 == t.cnf2 && cnf3 == t.cnf3);
  return true;
}

static void refreshErrorCounters() {
  lastRxError = evCan.errorCountRX();
  lastTxError = evCan.errorCountTX();
  lastErrorFlags = evCan.getErrorFlags();
}

void evCanRequestScan() {
  if (!evCanOk || scanActive) return;
  // Two fruitless sweeps in a row point at the controller itself rather than
  // the bitrate: pulse the reset line before looking again.
  if (emptySweeps >= 2) {
    emptySweeps = 0;
    evHardResets++;
    Serial.println("EV-CAN: repeated empty sweeps, hard-resetting MCP2515");
    digitalWrite(EVCAN_MCP_RST, LOW);
    delay(10);
    digitalWrite(EVCAN_MCP_RST, HIGH);
    delay(10);
    SPI.begin(EVCAN_SPI_SCLK, EVCAN_SPI_MISO, EVCAN_SPI_MOSI, EVCAN_SPI_CS);
    evCan.reset();
    uint8_t canstat = mcpReadRegister(0x0E);
    chipLooksValid = ((canstat & 0xE0) == 0x80);
  }
  timingLocked = false;
  scanActive = true;
  scanCompleted = false;
  scanStartedAt = millis();
  scanCursor = 0;
  scannedInPass = 0;
  scanPasses = 0;
  scanBestIndex = 0xFF;
  scanBestFrames = 0;
  for (uint8_t i = 0; i < kTimingCount; i++) scanResults[i] = 0;
  applyTiming(scanCursor);
  scanWindowStart = millis();
  scanWindowBaseline = canFramesA;
  Serial.println("EV-CAN: auto-detect started (clock/bitrate sweep, listen-only)");
}

static void finishScan() {
  scanActive = false;
  scanCompleted = true;
  lastScanEndMs = millis();
  if (scanBestFrames == 0) {
    emptySweeps++;
    // Do not keep a stored timing that produced nothing.
    storeTiming(0);
    activeTiming = 0;
  } else emptySweeps = 0;
  // Keep listening on the best-looking candidate, otherwise restore the
  // stored/default one so decoding resumes immediately when traffic starts.
  if (scanBestIndex < kTimingCount) activeTiming = scanBestIndex;
  applyTiming(activeTiming);
  refreshErrorCounters();
  Serial.printf("EV-CAN: sweep finished, no timing locked (best %s / %s: %lu frames)\n",
                kTimings[activeTiming].clockName, kTimings[activeTiming].speedName,
                (unsigned long)scanBestFrames);
  Serial.print("EV-CAN: per-candidate frames:");
  for (uint8_t i = 0; i < kTimingCount; i++) {
    const EvCanTiming& t = kTimings[i];
    if (i) Serial.print(",");
    Serial.printf(" %s/%s=%u", t.clockName, t.speedName, scanResults[i]);
  }
  Serial.println();
}

static void manageScan() {
  uint32_t now = millis();
  uint32_t windowMs = (now - scanStartedAt > kScanSlowAfterMs)
                        ? kScanWindowSlowMs : kScanWindowFastMs;
  if (now - scanWindowStart < windowMs) return;

  uint32_t frames = canFramesA - scanWindowBaseline;
  scanResults[scanCursor] = frames > 255 ? 255 : (uint8_t)frames;
  if (frames > scanBestFrames) {
    scanBestFrames = frames;
    scanBestIndex = scanCursor;
  }

  if (frames >= kLockFrames) {
    timingLocked = true;
    scanActive = false;
    scanCompleted = true;
    lastScanEndMs = now;
    activeTiming = scanCursor;
    storeTiming(activeTiming);
    refreshErrorCounters();
    Serial.printf("EV-CAN: locked on %s / %s (%lu frames in %lu ms, config CRC ok=%d)\n",
                  kTimings[activeTiming].clockName, kTimings[activeTiming].speedName,
                  (unsigned long)frames, (unsigned long)windowMs,
                  lastCfgWriteOk ? 1 : 0);
    return;
  }

  scanCursor = (uint8_t)((scanCursor + 1) % kTimingCount);
  scannedInPass++;
  if (scannedInPass >= kTimingCount) {
    // One full pass over every candidate produced nothing usable.
    scannedInPass = 0;
    scanPasses++;
    if (scanPasses >= 3) {
      finishScan();
      return;
    }
  }
  applyTiming(scanCursor);
  scanWindowStart = now;
  scanWindowBaseline = canFramesA;
}

void setupEvCAN() {
  pinMode(EVCAN_MCP_RST, OUTPUT);
  digitalWrite(EVCAN_MCP_RST, HIGH);
  delay(10);
  digitalWrite(EVCAN_MCP_RST, LOW);
  delay(10);
  digitalWrite(EVCAN_MCP_RST, HIGH);
  delay(10);

  SPI.begin(EVCAN_SPI_SCLK, EVCAN_SPI_MISO, EVCAN_SPI_MOSI, EVCAN_SPI_CS);

  if (evCan.reset() != MCP2515::ERROR_OK) {
    Serial.println("EV-CAN: MCP2515 not responding (check Port A wiring, 3V3, CS)");
    return;
  }
  // CANSTAT.OPMOD must read 0x80 (configuration mode) right after reset.
  // A board that answers SPI but not this pattern is not behaving like an
  // MCP2515, which matters because clones and MCP2518FD boards exist.
  uint8_t canstat = mcpReadRegister(0x0E);
  chipLooksValid = ((canstat & 0xE0) == 0x80);
  Serial.printf("EV-CAN: CANSTAT=0x%02X chip_check=%s\n",
                canstat, chipLooksValid ? "ok" : "unexpected");

  // Always start at 16 MHz / 500k: proven on this car and MCP2515. A stored
  // index from a false lock used to survive reboots and leave the board deaf.
  activeTiming = 0;
  if (!applyTiming(activeTiming)) {
    Serial.println("EV-CAN: setBitrate/setListenOnlyMode failed");
    return;
  }
  evCanOk = true;
  Serial.printf("EV-CAN: MCP2515 listen-only started (Port A, %s / %s, CNF ok=%d)\n",
                kTimings[activeTiming].clockName, kTimings[activeTiming].speedName,
                lastCfgWriteOk ? 1 : 0);
}

void manageEvCAN() {
  if (!evCanOk) return;
  // checkReceive() returns bool: comparing it against ERROR_OK (0) would be
  // always false and silently discard every frame. Keep the plain bool test.
  while (evCan.checkReceive()) {
    struct can_frame frame;
    if (evCan.readMessage(&frame) != MCP2515::ERROR_OK) break;
    canFramesA++;
    lastEvFrameMs = millis();
    evFrameSeen = true;
    canLoggerAppend('A', '<', frame.can_id, frame.data, frame.can_dlc);
    handleEvFrame(frame.can_id, frame.can_dlc, frame.data);
  }

  // RX0OVR/RX1OVR latch until cleared; a sticky flag looks like a live fault.
  uint8_t eflg = evCan.getErrorFlags();
  if (eflg & 0xC0) evCan.clearRXnOVRFlags();

  uint32_t now = millis();
  if (now - lastErrRefreshMs >= 2000) {
    lastErrRefreshMs = now;
    refreshErrorCounters();
  }

  if (scanActive) {
    manageScan();
    return;
  }

  if (!timingLocked) {
    if (canFramesA >= kLockFrames) {
      // Frames arrived with the stored/default timing: accept it.
      timingLocked = true;
      storeTiming(activeTiming);
      refreshErrorCounters();
      Serial.printf("EV-CAN: timing confirmed on boot: %s / %s\n",
                    kTimings[activeTiming].clockName, kTimings[activeTiming].speedName);
    } else if (evCanLastFrameAgeSec() > 20) {
      bool dueAfterBoot = !scanCompleted && millis() > kAutoScanAfterMs;
      bool dueAgain = scanCompleted && (millis() - lastScanEndMs > kRescanIntervalMs);
      if (dueAfterBoot || dueAgain) evCanRequestScan();
    }
  } else if (evCanLastFrameAgeSec() > kStaleLockSeconds) {
    // A wrong lock looks exactly like this: "locked" but the bus is silent
    // while the car is awake. Drop the stored timing and sweep again instead
    // of staying deaf until the next reboot.
    timingLocked = false;
    scanCompleted = false;
    Serial.printf("EV-CAN: no frames for %lu s, re-running timing sweep\n",
                  (unsigned long)kStaleLockSeconds);
    evCanRequestScan();
  }
}

bool evCanIsUp() {
  return evCanOk;
}

bool evCanChipCheckOk() {
  return chipLooksValid;
}

bool evCanConfigWritesOk() {
  return lastCfgWriteOk;
}

bool evCanIsLocked() {
  return timingLocked;
}

bool evCanScanActive() {
  return scanActive;
}

const char* evCanClockName() {
  return kTimings[scanActive ? scanCursor : activeTiming].clockName;
}

const char* evCanBitrateName() {
  return kTimings[scanActive ? scanCursor : activeTiming].speedName;
}

uint8_t evCanScanBestFrames() {
  return scanBestIndex < kTimingCount ? scanResults[scanBestIndex] : 0;
}

uint8_t evCanRxErrorCount() {
  return lastRxError;
}

uint8_t evCanTxErrorCount() {
  return lastTxError;
}

uint8_t evCanErrorFlags() {
  return lastErrorFlags;
}

uint32_t evCanHardResetCount() {
  return evHardResets;
}

uint8_t evCanRawPack(uint8_t* out) {
  if (out && rawPack1dbLen) memcpy(out, rawPack1db, 8);
  return rawPack1dbLen;
}

uint8_t evCanRawTemp(uint8_t* out) {
  if (out && rawTemp5c0Len) memcpy(out, rawTemp5c0, 8);
  return rawTemp5c0Len;
}

// Seconds since the last accepted EV-CAN frame; 0 = fresh, 0xFFFFFFFF = never.
uint32_t evCanLastFrameAgeSec() {
  if (!evFrameSeen) return 0xFFFFFFFFUL;
  return (millis() - lastEvFrameMs) / 1000UL;
}
