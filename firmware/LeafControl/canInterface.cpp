#include <Arduino.h>
#include "boardConfig.h"
#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
#include <SPI.h>
#include "mcp2515.h"
#else
#include "driver/twai.h"
#endif
#include "canInterface.h"
#include "canLogger.h"
#include "udsLeaf.h"

// Implemented in canLeafZE1.cpp. Declared here so the low-level CAN layer can
// gate the one unproven payload (OBD-II VIN read) without including the whole
// vehicle header.
bool vinObdProbeActive();

unsigned long logStartTime = 0;
// Per-frame serial logging costs ~3-5 ms per frame at 115200 — off by default.
// Enable temporarily via CLI: log on / log off
bool canLogFrames = false;
uint32_t canFramesB = 0;

#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
// --- Port B on a second MCP2515 ------------------------------------------
// T-Call: CAR-CAN is not TWAI anymore, it uses MCP2515 #1 on the same SPI bus
// as the EV-CAN controller. Each chip has its own CS line; the library toggles
// CS inside every SPI transaction, so the bus is safe to share.
//
// The controller is created in setupCAN(), after Serial.begin(). A global
// MCP2515 constructor would drive its CS line before the Arduino core is
// initialised; with an unpowered module on the bus that was enough to keep the
// board from printing anything at all.
static MCP2515* carCan = nullptr;
static bool carCanOk = false;
static CAN_CLOCK carCanClock = MCP_16MHZ;
static const char* carCanClockName = "16MHz";

// CANSTAT is the cheapest way to prove the chip is really there: a floating
// MISO reads 0x00/0xFF, a live controller in config mode reads 0x8X.
static uint8_t carMcpReadStat() {
  pinMode(BOARD_CAR_MCP_CS, OUTPUT);
  digitalWrite(BOARD_CAR_MCP_CS, HIGH);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(BOARD_CAR_MCP_CS, LOW);
  SPI.transfer(0x03);   // READ
  SPI.transfer(0x0E);   // CANSTAT
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(BOARD_CAR_MCP_CS, HIGH);
  SPI.endTransaction();
  return value;
}

static bool carCanApplyClock(CAN_CLOCK clock, const char* name) {
  if (!carCan) return false;
  if (carCan->reset() != MCP2515::ERROR_OK) return false;
  if (carCan->setBitrate(CAN_500KBPS, clock) != MCP2515::ERROR_OK) return false;
  if (carCan->setNormalMode() != MCP2515::ERROR_OK) return false;
  carCanClock = clock;
  carCanClockName = name;
  return true;
}
#else
// --- Port B on TWAI ------------------------------------------------------
#define CAN_TX_PIN BOARD_CAN_TX
#define CAN_RX_PIN BOARD_CAN_RX
static bool busOffRecoveryAttempted = false;

void print_can_message(twai_message_t &message) {
  unsigned long message_time = millis() - logStartTime;
  Serial.print(message_time / 1000.0, 3);
  Serial.print(" 0x");
  Serial.print(message.identifier, HEX);
  Serial.print(" ");
  for (int d = 0; d < message.data_length_code; d++) {
    Serial.print(" 0x");
    if (message.data[d] < 0x10) Serial.print("0");
    Serial.print(message.data[d], HEX);
  }
  Serial.println();
}
#endif

// Final TX interlock: ZE1 OVMS remote commands use only these exact frames.
// All other traffic is rejected here regardless of the transport.
static bool txAllowed(uint32_t id, const uint8_t* data, uint8_t len, bool bypassWhitelist) {
  static const uint8_t allowed56e[][4] = {
    {0x46, 0x08, 0x00, 0x00}, // wake/init
    {0x4E, 0x08, 0x12, 0x00}, // climate on
    {0x56, 0x00, 0x01, 0x00}, // climate off
    {0x66, 0x08, 0x12, 0x00}, // start charging
    {0x11, 0x00, 0x00, 0x00}, // unlock
    {0x60, 0x80, 0x00, 0x00}, // lock
    {0x46, 0x09, 0x12, 0x10}, // captured ZE1 light flash (RHL=1)
    {0x46, 0x0B, 0x12, 0x10}  // captured ZE1 horn + lights (RHL=3)
  };
  bool allowed = id == 0x68C && len == 1 && data && data[0] == 0x00;
  if (!allowed && udsWhitelistHit(id, data, len)) {
    // Tier-1 UDS ReadDataByIdentifier requests from udsLeaf.cpp: fixed
    // 03 22 <did> frames to the ECU addresses in its table.
    allowed = true;
  }
  if (id == 0x56E && len == 4 && data) {
    for (const auto& payload : allowed56e) {
      if (memcmp(data, payload, sizeof(payload)) == 0) {
        allowed = true;
        break;
      }
    }
  }
  if (id == 0x79B && len == 8 && data) {
    // Cell group 0x02 is no longer polled (its only consumer was the cell
    // delta tile, which was removed), so the whitelist stays at the groups we
    // actually ask for.
    static const uint8_t bmsTempRequest[8] = {0x02, 0x21, 0x04, 0, 0, 0, 0, 0};
    static const uint8_t bmsBatteryRequest[8] = {0x02, 0x21, 0x01, 0, 0, 0, 0, 0};
    static const uint8_t bmsSohRequest[8] = {0x02, 0x21, 0x61, 0, 0, 0, 0, 0};
    static const uint8_t isoTpFlowControl[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
    // STmin 20 ms: the 198-byte cell reply arrives back-to-back at 500 kbps in
    // ~4 ms, which the ESP32 loses whenever Wi-Fi/TLS work stalls the loop.
    static const uint8_t isoTpFlowControlSlow[8] = {0x30, 0x00, 0x14, 0, 0, 0, 0, 0};
    allowed = memcmp(data, bmsTempRequest, sizeof(bmsTempRequest)) == 0 ||
              memcmp(data, bmsBatteryRequest, sizeof(bmsBatteryRequest)) == 0 ||
              memcmp(data, bmsSohRequest, sizeof(bmsSohRequest)) == 0 ||
              memcmp(data, isoTpFlowControl, sizeof(isoTpFlowControl)) == 0 ||
              memcmp(data, isoTpFlowControlSlow, sizeof(isoTpFlowControlSlow)) == 0;
  }
  if (id == 0x797 && len == 8 && data) {
    // VIN read (UDS ReadDataByIdentifier 0x21 0x81) and its ISO-TP flow
    // control. Read-only diagnostic request, previously used on this car.
    static const uint8_t vinRequest[8] = {0x02, 0x21, 0x81, 0, 0, 0, 0, 0};
    static const uint8_t vinFlowControl[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
    allowed = memcmp(data, vinRequest, sizeof(vinRequest)) == 0 ||
              memcmp(data, vinFlowControl, sizeof(vinFlowControl)) == 0;
  }
  // Standard OBD-II VIN request (mode 09 PID 02) on the functional address.
  // The LBC answers the UDS variant with 17 blanks on this car, so the VCM is
  // the only remaining source. It stays blocked unless a developer probe is
  // running, which keeps the default whitelist exactly the OVMS-proven set.
  if (!allowed && vinObdProbeActive() && data && len == 8 &&
      (id == 0x7DF || id == 0x7E0)) {
    static const uint8_t vinObdRequest[8] = {0x02, 0x09, 0x02, 0, 0, 0, 0, 0};
    static const uint8_t vinObdFlowControl[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
    allowed = memcmp(data, vinObdRequest, sizeof(vinObdRequest)) == 0 ||
              memcmp(data, vinObdFlowControl, sizeof(vinObdFlowControl)) == 0;
  }
  if (!allowed && !bypassWhitelist) {
    Serial.printf("! CAN TX blocked by whitelist: 0x%03lX len=%u\n",
                  (unsigned long)id, len);
    canLoggerAppend('B', '!', id, data, len);
    return false;
  }
  return true;
}

// Helper function to send CAN message. Returns false if the frame was dropped
// (bus-off / stopped / TX queue full) so callers can retry the step.
static bool sendCANInternal(uint32_t id, const uint8_t* data, uint8_t len, bool bypassWhitelist) {
#if LEAF_CAN_RX_ONLY
  // Software interlock in addition to TWAI_MODE_LISTEN_ONLY. This blocks web,
  // MQTT, modem/call, timer refresh, VIN/ISO-TP and sequence TX paths.
  (void)id;
  (void)data;
  (void)len;
  (void)bypassWhitelist;
  return false;
#else
  if (!txAllowed(id, data, len, bypassWhitelist)) return false;
  if (len > 8) len = 8;

#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
  if (!carCanOk) return false;
  struct can_frame frame;
  memset(&frame, 0, sizeof(frame));
  frame.can_id = id;
  frame.can_dlc = len;
  memcpy(frame.data, data, len);
  bool ok = carCan->sendMessage(&frame) == MCP2515::ERROR_OK;
  canLoggerAppend('B', ok ? '>' : '!', id, data, len);
  if (canLogFrames) {
    Serial.printf("%s 0x%03lX len=%u\n", ok ? ">" : "!",
                  (unsigned long)id, (unsigned)len);
  }
  return ok;
#else
  twai_status_info_t status;
  if (twai_get_status_info(&status) == ESP_OK) {
    if (status.state == TWAI_STATE_BUS_OFF) {
      if (!busOffRecoveryAttempted) {
        Serial.println("! CAN bus-off detected, initiating recovery, TX skipped");
        twai_initiate_recovery();
        busOffRecoveryAttempted = true;
      }
      return false;
    }
    if (status.state == TWAI_STATE_STOPPED && busOffRecoveryAttempted) {
      if (twai_start() == ESP_OK) {
        Serial.println("CAN recovered from bus-off, driver restarted");
        busOffRecoveryAttempted = false;
      }
      return false;
    }
    busOffRecoveryAttempted = false;
  }

  twai_message_t message;
  message.identifier = id;
  message.extd = 0;
  message.rtr = 0;
  message.data_length_code = len;
  for (int i = 0; i < len; i++) {
    message.data[i] = data[i];
  }

  bool ok = (twai_transmit(&message, pdMS_TO_TICKS(10)) == ESP_OK);
  canLoggerAppend('B', ok ? '>' : '!', message.identifier, message.data, message.data_length_code);
  if (canLogFrames) {
    Serial.print(ok ? "> " : "! ");
    print_can_message(message);
  }
  return ok;
#endif
#endif
}

void setupCAN() {
#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
  // EV-CAN initialised the shared SPI bus first (setupEvCAN runs before this).
  // Re-assert every CS line, including the reserved third controller.
  if (!carCan) {
    carCan = new MCP2515(BOARD_CAR_MCP_CS, 4000000, &SPI);
  }
  if (!carCan) {
    Serial.println("CAR-CAN: cannot allocate MCP2515 driver");
    return;
  }
  pinMode(BOARD_CAR_MCP_CS, OUTPUT);
  digitalWrite(BOARD_CAR_MCP_CS, HIGH);
  pinMode(BOARD_EVCAN_SPI_CS, OUTPUT);
  digitalWrite(BOARD_EVCAN_SPI_CS, HIGH);
#if BOARD_AUXCAN_SPI_CS > 0
  pinMode(BOARD_AUXCAN_SPI_CS, OUTPUT);
  digitalWrite(BOARD_AUXCAN_SPI_CS, HIGH);
  Serial.printf("CAN: MCP2515 #3 reserved on CS%d (not driven)\n", BOARD_AUXCAN_SPI_CS);
#endif
  SPI.begin(BOARD_CAN_SPI_SCLK, BOARD_CAN_SPI_MISO, BOARD_CAN_SPI_MOSI);
  delay(5);
  // A warm reboot leaves the controller in normal mode; reset first so CANSTAT
  // can be used reliably as a presence check (config mode reads 0x8X).
  carCan->reset();
  uint8_t canstat = carMcpReadStat();
  if ((canstat & 0xE0) != 0x80) {
    carCanOk = false;
    Serial.printf("CAR-CAN: MCP2515 #1 not responding (CS%d SCK%d MOSI%d MISO%d, CANSTAT=0x%02X)\n",
                  BOARD_CAR_MCP_CS, BOARD_CAN_SPI_SCLK, BOARD_CAN_SPI_MOSI,
                  BOARD_CAN_SPI_MISO, canstat);
    return;
  }

  // Modules exist with 8, 16 and 20 MHz crystals. The old EV module on this
  // car is 16 MHz; BOARD_CAR_MCP_CLOCK is the single place to change it.
  if (carCanApplyClock(BOARD_CAR_MCP_CLOCK, BOARD_CAR_MCP_CLOCK_NAME)) {
    carCanOk = true;
    Serial.printf("CAR-CAN: MCP2515 #1 normal mode (Port B, 500k / %s, CS%d)\n",
                  carCanClockName, BOARD_CAR_MCP_CS);
  } else {
    Serial.println("CAR-CAN: MCP2515 #1 bitrate/mode setup failed");
  }
#else
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN,
#if LEAF_CAN_RX_ONLY
      TWAI_MODE_LISTEN_ONLY
#else
      TWAI_MODE_NORMAL
#endif
  );
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  g_config.rx_queue_len = 32; // diagnostics: 198-byte ISO-TP bursts plus traffic

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    Serial.println("CAN driver installed");
  } else {
    Serial.println("Failed to install CAN driver");
    return;
  }

  if (twai_start() == ESP_OK) {
#if LEAF_CAN_RX_ONLY
    Serial.println("CAN RX-ONLY: TWAI listen-only started (Port B, TX blocked)");
#else
    Serial.println("CAN command mode: Port B normal, TX whitelist active");
#endif
  } else {
    Serial.println("Failed to start CAN driver");
  }
#endif
}

void readAndHandleCANMessage() {
#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
  if (!carCanOk) return;
  int drained = 0;
  // Drain both MCP2515 receive buffers every loop pass (bounded).
  while (drained < 32 && carCan->checkReceive()) {
    struct can_frame frame;
    if (carCan->readMessage(&frame) != MCP2515::ERROR_OK) break;
    uint8_t len = frame.can_dlc > 8 ? 8 : frame.can_dlc;
    uint32_t id = frame.can_id & 0x1FFFFFFF;
    handleReceivedMessage(id, frame.data, len);
    canWatchRecord('B', id, frame.data, len);
    canFramesB++;
    canLoggerAppend('B', '<', id, frame.data, len);
    drained++;
  }
#else
  twai_message_t message;
  int drained = 0;
  // Drain the whole RX queue each loop pass (bounded) — a single read per
  // loop() overflows the queue on a busy bus.
  while (drained < 32 && twai_receive(&message, 0) == ESP_OK) {
    handleReceivedMessage(message.identifier, message.data, message.data_length_code);
    canWatchRecord('B', message.identifier, message.data, message.data_length_code);
    canFramesB++;
    canLoggerAppend('B', '<', message.identifier, message.data, message.data_length_code);
    if (canLogFrames) {
      Serial.print("< ");
      print_can_message(message);
    }
    drained++;
  }
#endif
}

// Keep Port B able to receive even when no command is transmitted.
void canMaintainBus() {
#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
  if (!carCanOk) return;
  // EFLG is an SPI read; once a second is plenty for error recovery.
  static uint32_t lastCheckMs = 0;
  uint32_t now = millis();
  if (now - lastCheckMs < 1000UL) return;
  lastCheckMs = now;
  uint8_t flags = carCan->getErrorFlags();
  if (flags & (MCP2515::EFLG_TXBO | MCP2515::EFLG_TXEP | MCP2515::EFLG_RXEP)) {
    Serial.printf("CAR-CAN: MCP2515 error flags 0x%02X, re-initialising\n", flags);
    if (!carCanApplyClock(carCanClock, carCanClockName)) {
      carCanOk = false;
      Serial.println("CAR-CAN: MCP2515 re-init failed");
    }
  } else if (flags & (MCP2515::EFLG_RX0OVR | MCP2515::EFLG_RX1OVR)) {
    carCan->clearRXnOVRFlags();
  }
#else
  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) return;
  if (status.state == TWAI_STATE_BUS_OFF) {
    Serial.println("! CAN bus-off, initiating recovery");
    twai_initiate_recovery();
    busOffRecoveryAttempted = true;
  } else if (status.state == TWAI_STATE_STOPPED) {
    if (twai_start() == ESP_OK) {
      Serial.println("CAN driver restarted after bus-off");
      busOffRecoveryAttempted = false;
    }
  } else if (busOffRecoveryAttempted) {
    busOffRecoveryAttempted = false;
  }
#endif
}

// Drain Port B for a bounded time, blocking the loop. Used only while an
// ISO-TP diagnostic reply is in flight: 198 bytes arrive back-to-back in a few
// milliseconds, so the main loop must not be doing Wi-Fi work meanwhile.
void canDrainFor(uint32_t ms) {
#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
  if (!carCanOk) return;
  uint32_t start = millis();
  do {
    while (carCan->checkReceive()) {
      struct can_frame frame;
      if (carCan->readMessage(&frame) != MCP2515::ERROR_OK) break;
      uint8_t len = frame.can_dlc > 8 ? 8 : frame.can_dlc;
      uint32_t id = frame.can_id & 0x1FFFFFFF;
      handleReceivedMessage(id, frame.data, len);
      canFramesB++;
      canLoggerAppend('B', '<', id, frame.data, len);
    }
    delay(1);
  } while (millis() - start < ms);
#else
  twai_message_t message;
  uint32_t start = millis();
  do {
    while (twai_receive(&message, 0) == ESP_OK) {
      handleReceivedMessage(message.identifier, message.data, message.data_length_code);
      canFramesB++;
      canLoggerAppend('B', '<', message.identifier, message.data, message.data_length_code);
    }
    delay(1);
  } while (millis() - start < ms);
#endif
}

uint8_t canBusState() {
#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
  if (!carCanOk) return 0xFF;
  uint8_t flags = carCan->getErrorFlags();
  if (flags & MCP2515::EFLG_TXBO) return 2;             // like TWAI_STATE_BUS_OFF
  if (flags & (MCP2515::EFLG_TXEP | MCP2515::EFLG_RXEP)) return 1;
  return 0;                                             // running
#else
  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) return 0xFF;
  return (uint8_t)status.state;
#endif
}

uint8_t canBusTxErrors() {
#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
  return carCanOk ? carCan->errorCountTX() : 0xFF;
#else
  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) return 0xFF;
  return status.tx_error_counter;
#endif
}

uint8_t canBusRxErrors() {
#if defined(BOARD_CAR_CAN_MCP2515) && (BOARD_CAR_CAN_MCP2515)
  return carCanOk ? carCan->errorCountRX() : 0xFF;
#else
  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) return 0xFF;
  return status.rx_error_counter;
#endif
}

bool sendCAN(uint32_t id, const uint8_t* data, uint8_t len) {
  return sendCANInternal(id, data, len, false);
}

// Research path: used only by the developer "saved commands" feature, which is
// armed explicitly for a short time from the web UI. Everything else in the
// firmware still goes through the whitelist above.
bool sendCANForced(uint32_t id, const uint8_t* data, uint8_t len) {
  return sendCANInternal(id, data, len, true);
}

void resetCanLogTimestamps() {
  logStartTime = millis();
}

// --- live CAN watch table -------------------------------------------------
// One row per (bus, identifier). The payload is compared with the previous
// frame: identical frames only refresh counters, real changes bump the change
// count and the timestamp, which is what the UI highlights.
struct CanWatchEntry {
  uint32_t id;
  uint8_t bus;
  uint8_t dlc;
  uint8_t data[8];
  uint32_t changes;
  uint32_t lastChangeMs;
  uint32_t firstSeenMs;
  bool used;
};

static const uint16_t CAN_WATCH_MAX = 128;
static CanWatchEntry canWatch[CAN_WATCH_MAX];

void canWatchReset() {
  memset(canWatch, 0, sizeof(canWatch));
}

void canWatchRecord(char bus, uint32_t id, const uint8_t* data, uint8_t dlc) {
  if (dlc > 8) dlc = 8;
  uint32_t now = millis();
  int16_t freeIndex = -1;
  int16_t oldestIndex = 0;
  for (uint16_t i = 0; i < CAN_WATCH_MAX; i++) {
    CanWatchEntry& e = canWatch[i];
    if (!e.used) {
      if (freeIndex < 0) freeIndex = (int16_t)i;
      continue;
    }
    if (e.id == id && e.bus == (uint8_t)bus) {
      if (e.dlc != dlc || memcmp(e.data, data, dlc) != 0) {
        e.dlc = dlc;
        memcpy(e.data, data, dlc);
        e.changes++;
        e.lastChangeMs = now;
      }
      return;
    }
    if ((int32_t)(now - canWatch[oldestIndex].lastChangeMs) >
        (int32_t)(now - e.lastChangeMs)) {
      oldestIndex = (int16_t)i;
    }
  }
  int16_t slot = freeIndex >= 0 ? freeIndex : oldestIndex;
  CanWatchEntry& e = canWatch[slot];
  e.id = id;
  e.bus = (uint8_t)bus;
  e.dlc = dlc;
  memcpy(e.data, data, dlc);
  e.changes = 1;
  e.lastChangeMs = now;
  e.firstSeenMs = now;
  e.used = true;
}

uint16_t canWatchCount() {
  uint16_t n = 0;
  for (uint16_t i = 0; i < CAN_WATCH_MAX; i++) if (canWatch[i].used) n++;
  return n;
}

bool canWatchGet(uint16_t index, CanWatchRow* out) {
  // Entries keep their insertion order so rows do not jump in the UI.
  uint16_t seen = 0;
  uint32_t now = millis();
  for (uint16_t i = 0; i < CAN_WATCH_MAX; i++) {
    const CanWatchEntry& e = canWatch[i];
    if (!e.used) continue;
    if (seen++ != index) continue;
    out->id = e.id;
    out->bus = (char)e.bus;
    out->dlc = e.dlc;
    memcpy(out->data, e.data, sizeof(out->data));
    out->changes = e.changes;
    out->ageMs = now - e.lastChangeMs;
    return true;
  }
  return false;
}
