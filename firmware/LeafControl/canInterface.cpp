#include <Arduino.h>
#include "driver/twai.h"
#include "canInterface.h"
#include "canLogger.h"

// --- CAN SETTINGS (Port B) ---
#define CAN_TX_PIN 7 
#define CAN_RX_PIN 6

unsigned long logStartTime = 0;
static bool busOffRecoveryAttempted = false;
// Per-frame serial logging costs ~3-5 ms per frame at 115200 — off by default.
// Enable temporarily via CLI: log on / log off
bool canLogFrames = false;
uint32_t canFramesB = 0;

void print_can_message(twai_message_t &message) {
      unsigned long message_time = millis() - logStartTime;
      Serial.print(message_time/1000.0, 3);
      Serial.print(" 0x");
      Serial.print(message.identifier, HEX);
      Serial.print(" ");
      for (int d = 0; d < message.data_length_code; d++) {
        Serial.print(" 0x");
        if (message.data[d] < 0x10) Serial.print("0");
        Serial.print(message.data[d], HEX);
        Serial.print(" ");
      }
      Serial.println();
}

// Helper function to send CAN message. Returns false if the frame was dropped
// (bus-off / stopped / TX queue full) so callers can retry the step.
bool sendCAN(uint32_t id,const uint8_t* data, uint8_t len) {
#if LEAF_CAN_RX_ONLY
  // Software interlock in addition to TWAI_MODE_LISTEN_ONLY. This blocks web,
  // MQTT, modem/call, timer refresh, VIN/ISO-TP and sequence TX paths.
  (void)id;
  (void)data;
  (void)len;
  return false;
#else
  // Final TX interlock: ZE1 OVMS remote commands use only the CAR-CAN wake
  // frame and these exact 0x56E payloads. All other traffic is rejected here.
  static const uint8_t allowed56e[][4] = {
    {0x46, 0x08, 0x00, 0x00}, // wake/init
    {0x4E, 0x08, 0x12, 0x00}, // climate on
    {0x56, 0x00, 0x01, 0x00}, // climate off
    {0x66, 0x08, 0x12, 0x00}, // start charging
    {0x11, 0x00, 0x00, 0x00}, // unlock
    {0x60, 0x80, 0x00, 0x00}, // lock
    {0x46, 0x09, 0x12, 0x10}  // captured ZE1 light flash (RHL=1)
  };
  bool allowed = id == 0x68C && len == 1 && data && data[0] == 0x00;
  if (id == 0x56E && len == 4 && data) {
    for (const auto& payload : allowed56e) {
      if (memcmp(data, payload, sizeof(payload)) == 0) {
        allowed = true;
        break;
      }
    }
  }
  if (id == 0x79B && len == 8 && data) {
    static const uint8_t bmsCellRequest[8] = {0x02, 0x21, 0x02, 0, 0, 0, 0, 0};
    static const uint8_t bmsTempRequest[8] = {0x02, 0x21, 0x04, 0, 0, 0, 0, 0};
    static const uint8_t isoTpFlowControl[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
    // STmin 20 ms: the 198-byte cell reply arrives back-to-back at 500 kbps in
    // ~4 ms, which the ESP32 loses whenever Wi-Fi/TLS work stalls the loop.
    static const uint8_t isoTpFlowControlSlow[8] = {0x30, 0x00, 0x14, 0, 0, 0, 0, 0};
    allowed = memcmp(data, bmsCellRequest, sizeof(bmsCellRequest)) == 0 ||
              memcmp(data, bmsTempRequest, sizeof(bmsTempRequest)) == 0 ||
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
  if (!allowed) {
    Serial.printf("! CAN TX blocked by whitelist: 0x%03lX len=%u\n",
                  (unsigned long)id, len);
    canLoggerAppend('B', '!', id, data, len);
    return false;
  }
  if (len > 8) len = 8;
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
}

void setupCAN() {
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
}

void readAndHandleCANMessage() {
  twai_message_t message;
  int drained = 0;
  // Drain the whole RX queue each loop pass (bounded) — a single read per
  // loop() overflows the queue on a busy bus.
  while (drained < 32 && twai_receive(&message, 0) == ESP_OK) {
    handleReceivedMessage(message.identifier, message.data, message.data_length_code);
    canFramesB++;
    canLoggerAppend('B', '<', message.identifier, message.data, message.data_length_code);
    if (canLogFrames) {
      Serial.print("< ");
      print_can_message(message);
    }
    drained++;
  }
}

// Keep Port B able to receive even when no command is transmitted. Bus-off
// recovery used to live only inside sendCAN(), so a controller that went
// bus-off while the car was asleep stayed deaf until the next command — and
// if that command also failed, indefinitely.
void canMaintainBus() {
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
}

// Drain Port B for a bounded time, blocking the loop. Used only while an
// ISO-TP diagnostic reply is in flight: 198 bytes arrive back-to-back in a few
// milliseconds, so the main loop must not be doing Wi-Fi work meanwhile.
void canDrainFor(uint32_t ms) {
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
}

uint8_t canBusState() {
  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) return 0xFF;
  return (uint8_t)status.state;
}

uint8_t canBusTxErrors() {
  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) return 0xFF;
  return status.tx_error_counter;
}

uint8_t canBusRxErrors() {
  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) return 0xFF;
  return status.rx_error_counter;
}

void resetCanLogTimestamps() {
  logStartTime = millis();
}
