#ifndef EV_CAN_LEAF_H
#define EV_CAN_LEAF_H

#include <Arduino.h>

// EV-CAN (battery bus) passive listener on T-2CAN Port A via MCP2515 SPI.
// Expected board: LilyGO T-2Can_V1.0 (MCP2515, 16 MHz crystal). Because a
// clone may use another crystal, the driver sweeps clock/bitrate candidates
// and locks onto the one that actually decodes frames.
// Listen-only: never transmits or ACKs on the battery bus.

void setupEvCAN();
void manageEvCAN();
bool evCanIsUp();

// Diagnostics / auto-detection
void evCanRequestScan();
bool evCanChipCheckOk();      // CANSTAT.OPMOD looked like a real MCP2515
bool evCanConfigWritesOk();   // CNF1/2/3 read back as written
bool evCanIsLocked();         // timing confirmed by received frames
bool evCanScanActive();       // sweep currently running
const char* evCanClockName();
const char* evCanBitrateName();
uint8_t evCanScanBestFrames();
uint8_t evCanRxErrorCount();
uint8_t evCanTxErrorCount();
uint8_t evCanErrorFlags();
uint32_t evCanLastFrameAgeSec();  // 0xFFFFFFFF = no frame yet
// Raw payloads from Port A for remote decoding (returns length, 0 = none)
uint8_t evCanRawPack(uint8_t* out);   // 0x1DB voltage/current
uint8_t evCanRawTemp(uint8_t* out);   // 0x5C0 battery temperature
uint32_t evCanHardResetCount();       // MCP2515 reset pulses after empty sweeps

// Callbacks (implemented in LeafControl.ino)
void handleEvSOH(int sohPercent);
void handleEvGids(int gids);
void handleEvPackStatus(float voltage, float current);
void handleEvAmbient(float ambientTemp);
void handleEvInstrumentSOC(float soc);
// BMS-reported SOC (diagnostic group 0x01), the honest ZE1 value.
void handleEvBmsSOC(float soc);
void handleEvVehicleOn(bool on);
void handleBatteryTemp(float temp);

#endif
