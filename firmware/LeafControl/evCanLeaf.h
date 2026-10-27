#ifndef EV_CAN_LEAF_H
#define EV_CAN_LEAF_H

#include <Arduino.h>

// EV-CAN (battery bus) passive listener on T-2CAN Port A via MCP2515 SPI.
// Board: LilyGO T-2Can_V1.0 (MCP2515, 16MHz crystal).
// Listen-only: never transmits or ACKs on the battery bus.

void setupEvCAN();
void manageEvCAN();
bool evCanIsUp();

// Callbacks (implemented in LeafControl.ino)
void handleEvSOH(int sohPercent);
void handleEvGids(int gids);
void handleEvPackStatus(float voltage, float current);
void handleEvAmbient(float ambientTemp);
void handleEvInstrumentSOC(float soc);
void handleEvVehicleOn(bool on);

#endif
