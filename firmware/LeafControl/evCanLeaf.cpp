#include "evCanLeaf.h"
#include <SPI.h>
#include "mcp2515.h"

// T-2CAN Port A pins (LilyGO pin_config.h)
#define EVCAN_SPI_SCLK 12
#define EVCAN_SPI_MISO 13
#define EVCAN_SPI_MOSI 11
#define EVCAN_SPI_CS   10
#define EVCAN_MCP_RST  9

static MCP2515 evCan(EVCAN_SPI_CS, 10000000, &SPI); // SPI clock 10MHz; setBitrate defaults to 16MHz crystal
static bool evCanOk = false;

static void handleEvFrame(uint32_t id, uint8_t len, const uint8_t* data) {
  if (id == 0x5BC && len >= 5) {
    // LB_Remain_Capacity_GIDS: start bit 7, len 10, motorola -> byte0<<2 | byte1>>6
    int gids = (data[0] << 2) | (data[1] >> 6);
    // LB_Capacity_Deterioration_Rate (SOH): start bit 33, len 7, intel -> byte4>>1
    int soh = (data[4] >> 1) & 0x7F;
    handleEvSOH(soh);
    handleEvGids(gids);
  } else if (id == 0x1DB && len >= 3) {
    // LB_Total_Voltage: start bit 23, len 10, 0.5V/bit -> byte2<<2 | byte1>>6
    int voltRaw = (data[2] << 2) | (data[1] >> 6);
    float voltage = voltRaw * 0.5f;
    // LB_Current: start bit 7, len 11 signed, 0.5A/bit -> byte0<<3 | byte1>>5
    int curRaw = (data[0] << 3) | (data[1] >> 5);
    if (curRaw & 0x400) curRaw -= 0x800; // sign-extend 11 bits
    float current = curRaw * 0.5f;
    handleEvPackStatus(voltage, current);
  } else if (id == 0x54C && len >= 7) {
    // OutsideAmbientTemperature: byte6, 0.5C/bit, offset -40 (OVMS xnl)
    float ambient = data[6] * 0.5f - 40.0f;
    if (data[6] != 0xFF) handleEvAmbient(ambient);
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
    Serial.println("EV-CAN: MCP2515 not responding (check Port A wiring)");
    return;
  }
  if (evCan.setBitrate(CAN_500KBPS) != MCP2515::ERROR_OK) {
    Serial.println("EV-CAN: setBitrate failed");
    return;
  }
  if (evCan.setListenOnlyMode() != MCP2515::ERROR_OK) {
    Serial.println("EV-CAN: setListenOnlyMode failed");
    return;
  }
  evCanOk = true;
  Serial.println("EV-CAN: MCP2515 listen-only started (Port A)");
}

void manageEvCAN() {
  if (!evCanOk) return;
  // Drain receive buffers (RXB0/RXB1); overflow flag ignored, frames are frequent
  while (evCan.checkReceive() == MCP2515::ERROR_OK) {
    struct can_frame frame;
    if (evCan.readMessage(&frame) != MCP2515::ERROR_OK) break;
    handleEvFrame(frame.can_id, frame.can_dlc, frame.data);
  }
}

bool evCanIsUp() {
  return evCanOk;
}
