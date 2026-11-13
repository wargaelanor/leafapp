#ifndef MODEM_HW_H
#define MODEM_HW_H

#include <Arduino.h>

class Client;

namespace modem800 {
void hwBegin();
bool hwTestAt(uint32_t timeoutMs);
bool hwSimReady();
bool hwSelectNetwork(const char* apn, const char* user);
bool hwNetworkConnected();
int16_t hwSignal();
bool hwGprsConnect(const char* apn, const char* user, const char* pass);
bool hwGprsDisconnect();
Client* hwTcpClient();
bool hwPrepareVoiceCalls();
bool hwNetworkTime(uint32_t* epoch);
void hwSetSleep(bool enable);
bool hwIncomingCaller(String* number, uint32_t timeoutMs);
void hwRejectCall();
bool hwAnswerAndPlay(uint8_t recordId);
bool hwEnableGps();
bool hwGetGps(float* lat, float* lon, float* alt, float* speed);

// HTTP через встроенный клиент SIM800 (AT+HTTP). TLS модуль не умеет,
// поэтому сервер принимает устройства на отдельном HTTP-порту.
// method: 0 = GET, 1 = POST. Возвращает true при коде 2xx/3xx.
bool hwHttp(int method, const char* url, const char* body, String& response, int& code);
void hwPulsePwrKey(int pin, bool powerOn);
}

namespace modem7670 {
void hwBegin();
bool hwTestAt(uint32_t timeoutMs);
bool hwSimReady();
bool hwSelectNetwork(const char* apn, const char* user);
bool hwNetworkConnected();
int16_t hwSignal();
bool hwGprsConnect(const char* apn, const char* user, const char* pass);
bool hwGprsDisconnect();
Client* hwTcpClient();
bool hwEnableGps();
bool hwGetGps(float* lat, float* lon, float* alt, float* speed);
void hwPulsePwrKey(int pin, bool powerOn);
}

#endif
