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
}

void setupCAN() {
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  g_config.rx_queue_len = 16; // default 5 overflows during wake bursts

  if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
    Serial.println("CAN driver installed");
  } else {
    Serial.println("Failed to install CAN driver");
    return;
  }

  if (twai_start() == ESP_OK) {
    Serial.println("CAN driver started");
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

void resetCanLogTimestamps() {
  logStartTime = millis();
}
