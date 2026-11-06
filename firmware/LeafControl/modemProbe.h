#ifndef MODEM_PROBE_H
#define MODEM_PROBE_H

#include <Arduino.h>

// Bench/diagnostic helper for a SIMCom modem (SIM800 or A7670E) wired to the
// modem UART. It scans the usual baud rates, reports the first one that answers
// "AT", then prints a few identity/SIM/network answers. It does not start a
// data connection and does not touch the CAN buses.
String modemProbeJson();

// Pulse PWRKEY (active low) when a pin is configured in the settings.
void modemProbePowerPulse();

// 0 when no PWRKEY pin is configured.
int modemProbePwrKeyPin();

#endif
