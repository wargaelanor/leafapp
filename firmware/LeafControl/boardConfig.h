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
#define BOARD_HAS_MODEM 1   // SIM800 stays on 26/27/32/33; CAN uses other pins

// CAR-CAN: built-in TWAI + MCP2551 transceiver.
#define BOARD_CAN_TX 14
#define BOARD_CAN_RX 35

// EV-CAN: MCP2515 over SPI + MCP2551 (listen-only, as on the T-2CAN).
// GPIO34/35 are input-only: 34 for MISO, 35 for CRX. GPIO25 is free for INT.
// These pins do not touch SIM800 (26/27/32/33/4/23/5), PMU (21/22), PSRAM
// (16/17), flash (6-11) or any strapping pin.
#define BOARD_EVCAN_SPI_SCLK 19
#define BOARD_EVCAN_SPI_MISO 34
#define BOARD_EVCAN_SPI_MOSI 18
#define BOARD_EVCAN_SPI_CS   13
#define BOARD_EVCAN_INT      25   // polled driver today, reserved for IRQ later
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
