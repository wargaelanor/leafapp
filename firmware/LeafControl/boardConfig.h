#ifndef LEAF_BOARD_CONFIG_H
#define LEAF_BOARD_CONFIG_H

// One firmware, two boards. The only thing that changes is this file's
// compile-time profile:
//   default            -> LilyGO T-2CAN (ESP32-S3, external SIM800)
//   -DLEAF_BOARD_TCALL=1 -> TTGO T-Call 1.4 (ESP32-WROVER-IB, onboard SIM800)

#if defined(LEAF_BOARD_TCALL) && (LEAF_BOARD_TCALL)
// -------------------------------------------------------------------------
// TTGO T-Call 1.4 (ESP32-WROVER-IB, 4 MB flash, 8 MB PSRAM)
// -------------------------------------------------------------------------
#define BOARD_NAME "TTGO T-Call 1.4"
#define BOARD_SHORT "tcall"
#define BOARD_HAS_PSRAM 1
#define BOARD_HAS_EVCAN 1
#define BOARD_HAS_IP5306 1
#define BOARD_HAS_MODEM 0   // CAN uses the SIM800 pins; onboard modem stays off

// CAR-CAN: built-in TWAI + MCP2551 transceiver.
#define BOARD_CAN_TX 32
#define BOARD_CAN_RX 33

// EV-CAN: MCP2515 over SPI + MCP2551 (listen-only, as on the T-2CAN).
// GPIO34/35 are input-only on ESP32 and cannot drive SCK/MOSI, so they are
// not used here. GPIO12 is the MTDI strapping pin and is avoided as well.
#define BOARD_EVCAN_SPI_SCLK 19
#define BOARD_EVCAN_SPI_MISO 35
#define BOARD_EVCAN_SPI_MOSI 18
#define BOARD_EVCAN_SPI_CS   13
#define BOARD_EVCAN_INT      14   // polled driver today, reserved for IRQ later
#define BOARD_EVCAN_MCP_RST  0    // 0 = tie RST to 3.3 V, no GPIO

// Onboard SIM800L (LilyGO T-Call v1.4 / IP5306 schematic).
#define BOARD_MODEM_RX 26
#define BOARD_MODEM_TX 27
#define BOARD_MODEM_RI 33
#define BOARD_MODEM_PWRKEY 4
#define BOARD_MODEM_POWER_ON 23
#define BOARD_MODEM_RESET 5
#define BOARD_MODEM_DTR 32
#define BOARD_PMU_SDA 21
#define BOARD_PMU_SCL 22

// 4 MB flash, OTA + a small history partition.
#define BOARD_TRIPS_LIMIT   (96UL * 1024UL)
#define BOARD_CHARGES_LIMIT (24UL * 1024UL)

#else
// -------------------------------------------------------------------------
// LilyGO T-2CAN (ESP32-S3, 16 MB flash, 8 MB OPI PSRAM)
// PSRAM stays disabled on this board because the external SIM800 uses
// GPIO35/37, which are the OPI PSRAM pins.
// -------------------------------------------------------------------------
#define BOARD_NAME "LilyGO T-2CAN"
#define BOARD_SHORT "t2can"
#define BOARD_HAS_PSRAM 0
#define BOARD_HAS_EVCAN 1
#define BOARD_HAS_IP5306 0
#define BOARD_HAS_MODEM 1

// CAR-CAN: built-in TWAI.
#define BOARD_CAN_TX 7
#define BOARD_CAN_RX 6

// EV-CAN: MCP2515 over SPI (Port A, listen-only).
#define BOARD_EVCAN_SPI_SCLK 12
#define BOARD_EVCAN_SPI_MISO 13
#define BOARD_EVCAN_SPI_MOSI 11
#define BOARD_EVCAN_SPI_CS   10
#define BOARD_EVCAN_MCP_RST  9

// External SIM800 on the 13-pin header.
#define BOARD_MODEM_RX 35
#define BOARD_MODEM_TX 38
#define BOARD_MODEM_RI 37
#define BOARD_MODEM_PWRKEY 0
#define BOARD_MODEM_POWER_ON 0
#define BOARD_MODEM_RESET 0
#define BOARD_MODEM_DTR 0
#define BOARD_PMU_SDA 0
#define BOARD_PMU_SCL 0

// 9 MB FAT in the 16 MB partition.
#define BOARD_TRIPS_LIMIT   (1024UL * 1024UL)
#define BOARD_CHARGES_LIMIT (256UL * 1024UL)

#endif

#endif
