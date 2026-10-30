#ifndef OVMS_V2_CLIENT_H
#define OVMS_V2_CLIENT_H

#include <Arduino.h>

void setupOvmsV2();
void manageOvmsV2();
void ovmsV2Disconnect();
bool ovmsV2IsConnected();
bool ovmsV2ParanoidIsActive();

// Implemented by LeafControl.ino. Returns 0=ok, 1=failed, 2=unsupported.
uint8_t handleOvmsV2Command(uint16_t command, const char* args);

#endif
