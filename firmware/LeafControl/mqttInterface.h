#ifndef MQTT_INTERFACE_H
#define MQTT_INTERFACE_H

#include <Arduino.h>
#include "vehicleTypes.h"

// Initialization and Loop
void setupMQTT();
void manageMQTT();
bool mqttIsConnected();
void mqttDisconnect();

// Status updates
void mqttUpdateSOC(float soc);
void mqttUpdateCabinTemp(float temp);
void mqttUpdateCharging(bool isCharging, ChargerState state);
void mqttUpdateHVAC(bool isOn);
void mqttUpdateHVACSetpoint(float setpoint);
void mqttUpdateFanSpeed(float speed);
void mqttUpdateHeatingMode(bool heating, bool cooling);
void mqttUpdateVentilationMode(VentilationMode mode);
void mqttUpdateDoors(bool fl, bool fr, bool rl, bool rr, bool trunk);
void mqttUpdateLock(bool locked);
void mqttUpdateSOH(int soh);
void mqttUpdateGids(int gids);
void mqttUpdatePackStatus(float voltage, float current);
void mqttUpdateAmbient(float ambientTemp);
void mqttUpdateInstrumentSoc(float soc);
void mqttUpdateInstrumentRange(int km);
void mqttUpdateBatteryTemp(float temp);
void mqttUpdateCellDelta(int mv);
void mqttUpdateVehicleOn(bool on);
void mqttUpdateHeadlights(bool on);
void mqttUpdateGPS(float lat, float lon, float alt, float speed);
void mqttPublishStatus(const char* msg);

// Optional callbacks implemented in LeafControl.ino
extern void handleMqttHvacOn(float setpoint);
extern void handleMqttHvacOff();
extern void handleMqttChargeOn();
extern void handleMqttChargeOff();
extern bool handleMqttRefresh();
extern void handleMqttLock();
extern void handleMqttUnlock();
extern void handleMqttLights();
extern void handleMqttHornLights();
extern void handleMqttRhlOff();

#endif
