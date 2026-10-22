#include "modemHw.h"

#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>

static TinyGsm modem(Serial1);
static TinyGsmSim800::GsmClientSim800 tcpClient(modem, 0);

namespace modem800 {

void hwBegin() {
  Serial1.begin(115200, SERIAL_8N1, 44, 43);
}

bool hwTestAt(uint32_t timeoutMs) {
  return modem.testAT(timeoutMs);
}

bool hwSimReady() {
  return modem.getSimStatus() == SIM_READY;
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
  return false;
}

bool hwGetGps(float* lat, float* lon, float* alt, float* speed) {
  return false;
}

void hwPulsePwrKey(int pin, bool powerOn) {
  if (pin <= 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(powerOn ? 1200 : 1800);
  digitalWrite(pin, HIGH);
}

}
