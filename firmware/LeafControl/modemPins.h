#ifndef MODEM_PINS_H
#define MODEM_PINS_H

// The T-2CAN carries OPI PSRAM, which occupies GPIO 33-37. The modem UART
// therefore lives on free header pins instead of the old 35/38 pair:
//   ESP32 RX (GPIO18) <- modem TX
//   ESP32 TX (GPIO17) -> modem RX
//   RI      (GPIO16) <- modem RI (optional, can be left unconnected)
// Only one modem is connected at a time; the SIM800 and the A7670 share these
// pins, so the wiring is the same for both.
#define MODEM_UART_RX 18
#define MODEM_UART_TX 17
#define MODEM_RI_PIN  16

#endif
