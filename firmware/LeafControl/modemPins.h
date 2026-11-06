#ifndef MODEM_PINS_H
#define MODEM_PINS_H

#include "boardConfig.h"

// Modem UART/power pins are board-specific and live in boardConfig.h.
// T-2CAN: external SIM800 on 35/38 (RI 37).
// T-Call: onboard SIM800L on 26/27, RI 33, PWRKEY 4, POWER_ON 23, RST 5.
#define MODEM_UART_RX BOARD_MODEM_RX
#define MODEM_UART_TX BOARD_MODEM_TX
#define MODEM_RI_PIN  BOARD_MODEM_RI

#endif
