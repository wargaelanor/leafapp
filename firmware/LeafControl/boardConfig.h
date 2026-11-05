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

// T-Call: both CAN channels use external MCP2515 controllers on one SPI bus.
// The onboard TWAI pins (14/25) are not used in this profile.
//
// IMPORTANT: the SIM800/IP5306 own GPIO4 (PWRKEY), 5 (RESET), 21/22 (PMU I2C),
// 23 (POWER_ON), 26/27 (UART), 32 (DTR) and 33 (RI). SPI must never touch
// them: GPIO5/21/23 in the draft wiring would reset the modem or kill the PMU.
// Free pins after switching CAR-CAN from TWAI to MCP2515: 13, 14, 15, 18, 19,
// 25. GPIO34 is input-only and also free. GPIO13 drives the onboard LED, so it
// is left alone; GPIO15 is a strapping pin and is only used for the reserved
// third controller (add its 10 kOhm CS pull-up when you wire it).
#define BOARD_CAR_CAN_MCP2515 1
#define BOARD_CAN_SPI_SCLK 18
#define BOARD_CAN_SPI_MISO 34    // input-only pad, exactly what MISO needs
#define BOARD_CAN_SPI_MOSI 19
#define BOARD_CAR_MCP_CS   25    // MCP2515 #1: CAR-CAN (commands, normal mode)
#define BOARD_EVCAN_SPI_CS 14    // MCP2515 #2: EV-CAN (listen-only)
#define BOARD_AUXCAN_SPI_CS 15   // MCP2515 #3: reserved for the future, not driven

#define BOARD_EVCAN_SPI_SCLK BOARD_CAN_SPI_SCLK
#define BOARD_EVCAN_SPI_MISO BOARD_CAN_SPI_MISO
#define BOARD_EVCAN_SPI_MOSI BOARD_CAN_SPI_MOSI
#define BOARD_EVCAN_INT      0    // polled driver; do not wire INT
#define BOARD_EVCAN_MCP_RST  0    // 0 = tie RST to 3.3 V, no GPIO

// Crystal on the MCP2515 modules. 16 MHz matches the module already used on
// this car; the common blue clone boards often carry 8 MHz. If CAR-CAN stays
// silent while the transceiver is wired, change this to MCP_8MHZ and rebuild.
#define BOARD_CAR_MCP_CLOCK      MCP_16MHZ
#define BOARD_CAR_MCP_CLOCK_NAME "16MHz"

// Not used by this profile; kept defined so shared diagnostics compile.
#define BOARD_CAN_TX 0
#define BOARD_CAN_RX 0

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
#define BOARD_CAR_CAN_MCP2515 0
#define BOARD_CAN_TX 7
#define BOARD_CAN_RX 6

// EV-CAN: MCP2515 over SPI (Port A, listen-only).
#define BOARD_EVCAN_SPI_SCLK 12
#define BOARD_EVCAN_SPI_MISO 13
#define BOARD_EVCAN_SPI_MOSI 11
#define BOARD_EVCAN_SPI_CS   10
#define BOARD_EVCAN_MCP_RST  9
#define BOARD_AUXCAN_SPI_CS  0    // no third controller on this board

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
