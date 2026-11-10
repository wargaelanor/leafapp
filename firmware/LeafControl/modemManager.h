#ifndef MODEM_MANAGER_H
#define MODEM_MANAGER_H

#include <Arduino.h>

class Client;

enum class ModemState : uint8_t {
  OFF = 0,
  STARTING,
  READY,
  STANDBY,
  CALL_ACTIVE,
  FAILED,
  STOPPING
};

void modemInit();
void modemTick();
bool modemDataReady();
Client* modemTcpClient();
void modemPowerOffNow();
bool modemGpsFix(float* lat, float* lon, float* alt, float* speed, uint32_t* ageMs);
// Диагностика модема забирает Serial2 себе: менеджер на это время молчит.
void modemManagerPause(bool paused);
bool modemTaskBusy();

ModemState modemGetState();
const char* modemGetStateStr();

#endif
