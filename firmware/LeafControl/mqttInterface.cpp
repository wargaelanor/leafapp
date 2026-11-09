#include "mqttInterface.h"

// MQTT-транспорт удалён: плата работает только со своим сервером (serverApi).
// Файл оставлен как совместимый слой — mqttUpdate* вызываются из парсеров CAN
// и больше никуда не публикуют. Никаких сетевых подключений здесь нет.

void setupMQTT() {}
void manageMQTT() {}
bool mqttIsConnected() { return false; }
void mqttDisconnect() {}
void mqttGoingOffline() {}

void mqttUpdateSOC(float) {}
void mqttUpdateCabinTemp(float) {}
void mqttUpdateCharging(bool, ChargerState) {}
void mqttUpdateHVAC(bool) {}
void mqttUpdateHVACSetpoint(float) {}
void mqttUpdateFanSpeed(float) {}
void mqttUpdateHeatingMode(bool, bool) {}
void mqttUpdateVentilationMode(VentilationMode) {}
void mqttUpdateDoors(bool, bool, bool, bool, bool) {}
void mqttUpdateLock(bool) {}
void mqttUpdateSOH(int) {}
void mqttUpdateGids(int) {}
void mqttUpdatePackStatus(float, float, bool) {}
void mqttUpdateOdometer(uint32_t) {}
void mqttUpdateVin(const char*) {}
void mqttUpdateHvacTimer(uint16_t) {}
void mqttUpdateAmbient(float) {}
void mqttUpdateInstrumentSoc(float) {}
void mqttUpdateInstrumentRange(int) {}
void mqttUpdateBatteryTemp(float) {}
void mqttUpdateCellDelta(int) {}
void mqttUpdateVehicleOn(bool) {}
void mqttUpdateHeadlights(bool) {}
void mqttUpdateGPS(float, float, float, float) {}
void mqttPublishStatus(const char*) {}
