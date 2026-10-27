#include "modemHw.h"

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>

static TinyGsm modem(Serial2);
static TinyGsmSim7600::GsmClientSim7600 tcpClient(modem, 0);

namespace modem7670 {

void hwBegin() {
  Serial2.begin(115200, SERIAL_8N1, 1, 2);
}

bool hwTestAt(uint32_t timeoutMs) {
  return modem.testAT(timeoutMs);
}

bool hwSimReady() {
  return modem.getSimStatus() == SIM_READY;
}

bool hwSelectNetwork(const char*, const char*) {
  return true;
}

bool hwNetworkConnected() {
  return modem.isNetworkConnected();
}

int16_t hwSignal() {
  return modem.getSignalQuality();
}

bool hwGprsConnect(const char* apn, const char* user, const char* pass) {
  return modem.gprsConnect(apn, user, pass);
}

bool hwGprsDisconnect() {
  return modem.gprsDisconnect();
}

Client* hwTcpClient() {
  return &tcpClient;
}

bool hwEnableGps() {
  return modem.enableGPS();
}

bool hwGetGps(float* lat, float* lon, float* alt, float* speed) {
  return modem.getGPS(lat, lon, speed, alt);
}

void hwPulsePwrKey(int pin, bool powerOn) {
  if (pin <= 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(powerOn ? 600 : 3000);
  digitalWrite(pin, HIGH);
}

}
