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
    ERROR_PORT_RANGE,
    ERROR_INVALID,
    ERROR_NVS_WRITE
};

void initConfiguration();
void factoryReset();

enum class RemoteTransport : uint8_t {
  // Локальный режим: полный веб-интерфейс на самой плате, модем только
  // принимает звонки и не выходит в интернет.
  LOCAL = 0,
  // Режим сервера: лёгкий интерфейс, постоянная связь с сервером, звонки тоже.
  SERVER = 1
};

RemoteTransport getRemoteTransport();
ConfigStatus setRemoteTransport(RemoteTransport value);

const char* getWifiSSID();
ConfigStatus setWifiSSID(const char* value);

const char* getWifiPassword();
ConfigStatus setWifiPassword(const char* value);

const char* getWifiSSID2();
ConfigStatus setWifiSSID2(const char* value);
const char* getWifiPassword2();
ConfigStatus setWifiPassword2(const char* value);

const char* getWifiSSID3();
ConfigStatus setWifiSSID3(const char* value);
const char* getWifiPassword3();
ConfigStatus setWifiPassword3(const char* value);

const char* getApSSID();
ConfigStatus setApSSID(const char* value);

const char* getApPassword();
ConfigStatus setApPassword(const char* value);


const char* getHostName();
ConfigStatus setHostName(const char* value);

bool getLockingEnabled();
ConfigStatus setLockingEnabled(bool value);

// true = корневая страница отдаёт лёгкий интерфейс, false = полный.
bool getUiLite();
ConfigStatus setUiLite(bool value);

bool getSleepEnabled();
ConfigStatus setSleepEnabled(bool value);

// Профиль машины: 0 = авто, 1 = ZE1 (2018+), 2 = AZE0/ZE0 (2011-2017).
int getVehicleProfile();
void setVehicleProfile(int value);

int getSleepAfterHours();
ConfigStatus setSleepAfterHours(int value);
int getWakeFastMinutes();
ConfigStatus setWakeFastMinutes(int value);
int getWakeSlowMinutes();
ConfigStatus setWakeSlowMinutes(int value);

bool getUdsEnabled();
ConfigStatus setUdsEnabled(bool value);
bool getUdsTpmsEnabled();
ConfigStatus setUdsTpmsEnabled(bool value);

// Подсказка восстановления Wi-Fi: поднимать точку доступа при загрузке,
// если прошлая сессия закончилась недоступностью сохранённой сети.
bool getWifiRecoveryHint();
ConfigStatus setWifiRecoveryHint(bool value);

// Автостоп зарядки по порогу SOC (0 = выключено, иначе 70..95).
int getChargeLimitPct();
ConfigStatus setChargeLimitPct(int value);

// Сотовый модем (SIM800 / SIMCom A7670E); type: 0=SIM800, 1=A7670
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

// Управление климатом входящим звонком. RI закреплён на GPIO37.
bool getModemCallEnabled();
ConfigStatus setModemCallEnabled(bool value);
const char* getModemCallNumber(uint8_t index);
ConfigStatus setModemCallNumber(uint8_t index, const char* value);

#endif

