#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <Arduino.h>

enum class ConfigStatus {
    OK,
    ERROR_UNKNOWN,
    ERROR_LENGTH,
    ERROR_PASSWORD_LENGTH,
    ERROR_HOSTNAME_LENGTH,
    ERROR_HOSTNAME_HYPHEN,
    ERROR_HOSTNAME_CHARS,
    ERROR_PORT_RANGE
};

void initConfiguration();
void factoryReset();

const char* getWifiSSID();
ConfigStatus setWifiSSID(const char* value);

const char* getWifiPassword();
ConfigStatus setWifiPassword(const char* value);

const char* getApSSID();
ConfigStatus setApSSID(const char* value);

const char* getApPassword();
ConfigStatus setApPassword(const char* value);

const char* getHostName();
ConfigStatus setHostName(const char* value);

const char* getMqttServer();
ConfigStatus setMqttServer(const char* value);

int getMqttPort();
ConfigStatus setMqttPort(int value);

const char* getMqttUser();
ConfigStatus setMqttUser(const char* value);

const char* getMqttPassword();
ConfigStatus setMqttPassword(const char* value);

const char* getVehicleId();
ConfigStatus setVehicleId(const char* value);

bool getLockingEnabled();
ConfigStatus setLockingEnabled(bool value);

bool getMqttTls();
ConfigStatus setMqttTls(bool value);

bool getHassDiscoveryEnabled();
ConfigStatus setHassDiscoveryEnabled(bool value);

bool getSleepEnabled();
ConfigStatus setSleepEnabled(bool value);

int getSleepAfterHours();
ConfigStatus setSleepAfterHours(int value);

int getWakeFastMinutes();
ConfigStatus setWakeFastMinutes(int value);

int getWakeSlowMinutes();
ConfigStatus setWakeSlowMinutes(int value);

// Cellular modem (SIM800 / SIMCom A7670E); type: 0=SIM800, 1=A7670
bool getModemEnable();
ConfigStatus setModemEnable(bool value);

int getModemType();
ConfigStatus setModemType(int value);

int getModemPwrKey();
ConfigStatus setModemPwrKey(int value);

const char* getModem800Apn();
ConfigStatus setModem800Apn(const char* value);

const char* getModem800User();
ConfigStatus setModem800User(const char* value);

const char* getModem800Pass();
ConfigStatus setModem800Pass(const char* value);

const char* getModem7670Apn();
ConfigStatus setModem7670Apn(const char* value);

const char* getModem7670User();
ConfigStatus setModem7670User(const char* value);

const char* getModem7670Pass();
ConfigStatus setModem7670Pass(const char* value);

bool getModem7670Gps();
ConfigStatus setModem7670Gps(bool value);

#endif
