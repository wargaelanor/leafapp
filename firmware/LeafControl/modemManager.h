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
bool modemMqttReady();
Client* modemTcpClient();
void modemPowerOffNow();
bool modemGpsFix(float* lat, float* lon, float* alt, float* speed, uint32_t* ageMs);
ModemState modemGetState();
const char* modemGetStateStr();

#endif
