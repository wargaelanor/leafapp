#include "ovmsV2Client.h"
#include "configuration.h"
#include "vehicleTypes.h"
#include "modemManager.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>

extern float currentSOC;
extern float cabinTemp;
extern bool isChargingNow;
extern bool isHvacOn;
extern int8_t lockState;
extern bool carIsAwake;
extern bool doorFL, doorFR, doorRL, doorRR, doorTrunk;
extern EvBatteryData evData;

namespace {
WiFiClient plainClient;
WiFiClientSecure* tlsClient = nullptr;
Client* client = nullptr;
bool authenticated = false;
unsigned long lastConnectAttempt = 0;
unsigned long lastTransmit = 0;
unsigned long connectedAt = 0;
unsigned long lastReceive = 0;
unsigned long lastPing = 0;
unsigned long nextSnapshotAt = 0;
unsigned long nextSnapshotPartAt = 0;
uint8_t snapshotPart = 5;
String rxLine;
String clientToken;
String paranoidToken;
uint8_t paranoidDigest[16] = {};
bool paranoidReady = false;

struct Rc4State {
  uint8_t s[256];
  uint8_t i = 0;
  uint8_t j = 0;
};
Rc4State txRc4, rxRc4;

void rc4Init(Rc4State& state, const uint8_t* key, size_t keyLen) {
  for (int i = 0; i < 256; i++) state.s[i] = (uint8_t)i;
  uint8_t j = 0;
  for (int i = 0; i < 256; i++) {
    j = (uint8_t)(j + state.s[i] + key[i % keyLen]);
    uint8_t t = state.s[i]; state.s[i] = state.s[j]; state.s[j] = t;
  }
  state.i = state.j = 0;
}

void rc4Crypt(Rc4State& state, uint8_t* data, size_t len) {
  for (size_t n = 0; n < len; n++) {
    state.i = (uint8_t)(state.i + 1);
    state.j = (uint8_t)(state.j + state.s[state.i]);
    uint8_t t = state.s[state.i]; state.s[state.i] = state.s[state.j]; state.s[state.j] = t;
    data[n] ^= state.s[(uint8_t)(state.s[state.i] + state.s[state.j])];
  }
}

bool hmacMd5(const String& data, const char* password, uint8_t digest[16]) {
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
  return md && mbedtls_md_hmac(md, (const uint8_t*)password, strlen(password),
      (const uint8_t*)data.c_str(), data.length(), digest) == 0;
}

String mpEncode(const String& value) {
  String encoded;
  encoded.reserve(value.length());
  char previous = 0;
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '\n') {
      if (previous != '\r') encoded += '\r';
    } else if (c == ',') {
      encoded += ';';
    } else {
      encoded += c;
    }
    previous = c;
  }
  return encoded;
}

String base64Encode(const uint8_t* data, size_t len) {
  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
  String out;
  if (!out.reserve(outLen + 1)) return String();
  uint8_t* buffer = new (std::nothrow) uint8_t[outLen + 1];
  if (!buffer) return String();
  if (mbedtls_base64_encode(buffer, outLen + 1, &outLen, data, len) == 0) {
    buffer[outLen] = 0;
    out = (char*)buffer;
  }
  delete[] buffer;
  return out;
}

bool base64Decode(const String& input, uint8_t*& data, size_t& len) {
  size_t required = 0;
  mbedtls_base64_decode(nullptr, 0, &required, (const uint8_t*)input.c_str(), input.length());
  data = new (std::nothrow) uint8_t[required + 1];
  if (!data) return false;
  if (mbedtls_base64_decode(data, required, &len, (const uint8_t*)input.c_str(), input.length()) != 0) {
    delete[] data; data = nullptr; return false;
  }
  data[len] = 0;
  return true;
}

void resetConnection() {
  if (client) client->stop();
  client = nullptr;
  if (tlsClient) {
    delete tlsClient;
    tlsClient = nullptr;
  }
  authenticated = false;
  connectedAt = 0;
  lastReceive = 0;
  lastPing = 0;
  nextSnapshotAt = 0;
  nextSnapshotPartAt = 0;
  snapshotPart = 5;
  paranoidReady = false;
  paranoidToken = "";
  memset(paranoidDigest, 0, sizeof(paranoidDigest));
  rxLine = "";
}

bool transmitTransport(const String& message) {
  if (!authenticated || !client || !client->connected()) return false;
  size_t len = message.length();
  uint8_t* encrypted = new (std::nothrow) uint8_t[len];
  if (!encrypted) return false;
  memcpy(encrypted, message.c_str(), len);
  rc4Crypt(txRc4, encrypted, len);
  String encoded = base64Encode(encrypted, len);
  delete[] encrypted;
  if (!encoded.length()) return false;
  encoded += "\r\n";
  return client->write((const uint8_t*)encoded.c_str(), encoded.length()) == encoded.length();
}

bool transmit(const String& message) {
  if (!paranoidReady || message.length() < 6 || message[5] == 'E' ||
      message[5] == 'A' || message[5] == 'a' || message[5] == 'g' || message[5] == 'P') {
    return transmitTransport(message);
  }

  const size_t payloadLen = message.length() - 6;
  uint8_t* payload = new (std::nothrow) uint8_t[payloadLen];
  if (!payload && payloadLen > 0) return false;
  if (payloadLen > 0) memcpy(payload, message.c_str() + 6, payloadLen);
  Rc4State paranoidRc4;
  rc4Init(paranoidRc4, paranoidDigest, sizeof(paranoidDigest));
  uint8_t discard[1024] = {};
  rc4Crypt(paranoidRc4, discard, sizeof(discard));
  if (payloadLen > 0) rc4Crypt(paranoidRc4, payload, payloadLen);
  String wrapped = "MP-0 EM";
  wrapped += message[5];
  wrapped += base64Encode(payload, payloadLen);
  delete[] payload;
  return transmitTransport(wrapped);
}

bool sendStatus() {
  char msg[420];
  const float soc = currentSOC >= 0 ? currentSOC : -1;
  const int range = evData.instrumentRange >= 0 ? evData.instrumentRange : -1;
  const float voltage = evData.packVoltageValid ? evData.packVoltage : -1;
  const float current = evData.packDataValid ? evData.packCurrent : 0;
  const float power = evData.packDataValid ? evData.packPower : 0;
  const int soh = evData.soh >= 0 ? evData.soh : -1;
  snprintf(msg, sizeof(msg),
      "MP-0 S%.1f,K,0,0,%s,standard,%d,%d,0,0,0,0,0,0,0,0,0,-1,0,0,0,0,-1,0,0,0,0,0,%d,0,%.1f,%.1f,%d,0,0,%.1f,0,0,0,0,-1",
      soc, isChargingNow ? "charging" : "stopped", range, range, range,
      isChargingNow ? -power : 0, voltage, soh, current);
  return transmit(msg);
}

bool sendEnvironment() {
  bool hasState = currentSOC >= 0 || evData.vehicleOn >= 0 || lockState >= 0 ||
      carIsAwake || isChargingNow || isHvacOn || evData.packDataValid ||
      evData.batteryTemp > -90 || evData.ambientTemp > -90 || cabinTemp > -30 ||
      evData.odometerKm > 0 || evData.vin[0] != '\0';
  if (!hasState) return true;
  uint8_t doors1 = (doorFL ? 1 : 0) | (doorFR ? 2 : 0) |
      (isChargingNow ? 0x10 : 0) | (evData.vehicleOn == 1 ? 0x40 : 0);
  uint8_t doors2 = (lockState == 1 ? 0x08 : 0) | (doorTrunk ? 0x80 : 0);
  uint8_t doors3 = carIsAwake ? 1 : 0;
  uint8_t doors5 = (doorRL ? 1 : 0) | (doorRR ? 2 : 0) | (isHvacOn ? 0x80 : 0);
  bool temperaturesValid = evData.batteryTemp > -90 || evData.ambientTemp > -90 || cabinTemp > -30;
  char msg[300];
  snprintf(msg, sizeof(msg), "MP-0 D%u,%u,%d,0,0,%.1f,0,%lu,0,0,%.1f,%u,%d,%d,0,0,0,%u,0,0,%.1f",
      doors1, doors2, lockState == 1 ? 4 : (lockState == 0 ? 5 : 0),
      evData.batteryTemp > -90 ? evData.batteryTemp : 0,
      (unsigned long)evData.odometerKm * 10UL,
      evData.ambientTemp > -90 ? evData.ambientTemp : 0,
      doors3, temperaturesValid ? 1 : 0, evData.ambientTemp > -90 ? 1 : 0, doors5,
      cabinTemp > -30 ? cabinTemp : 0);
  return transmit(msg);
}

bool sendFirmware() {
  char msg[220];
  String network = mpEncode(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("SIM800"));
  String mode = mpEncode(WiFi.status() == WL_CONNECTED ? String("WiFi,Online") : String("Cellular,Online"));
  snprintf(msg, sizeof(msg), "MP-0 FLeafControl,%s,%d,1,NL,%s,-1,-1,T-2CAN,%s",
      evData.vin, WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
      network.c_str(), mode.c_str());
  return transmit(msg);
}

bool sendCapabilities() {
  return transmit("MP-0 VC11,C18-20,C22,C24,C26");
}

bool sendGps() {
  float lat = 0, lon = 0, alt = 0, speed = 0;
  uint32_t age = 0;
  bool valid = modemGpsFix(&lat, &lon, &alt, &speed, &age);
  if (!valid) return true;
  char msg[220];
  snprintf(msg, sizeof(msg), "MP-0 L%.6f,%.6f,0,%.0f,%d,%d,%.1f,0,0,%.3f,0,0,0,0,0,0,0,0,0",
      lat, lon, alt, valid ? 1 : 0, valid && age < 120000 ? 1 : 0, speed,
      evData.packDataValid ? evData.packPower : 0);
  return transmit(msg);
}

void startSnapshot() {
  snapshotPart = 0;
  nextSnapshotPartAt = millis();
}

bool sendNextSnapshotPart() {
  bool sent = false;
  switch (snapshotPart) {
    case 0: sent = sendStatus(); break;
    case 1: sent = sendEnvironment(); break;
    case 2: sent = sendGps(); break;
    case 3: sent = sendFirmware(); break;
    case 4: sent = sendCapabilities(); break;
    default: return true;
  }
  if (!sent) return false;
  lastTransmit = millis();
  snapshotPart++;
  nextSnapshotPartAt = millis() + 250;
  if (snapshotPart >= 5) nextSnapshotAt = millis() + 60000;
  return true;
}

void processCommand(const char* payload) {
  char* end = nullptr;
  unsigned long command = strtoul(payload, &end, 10);
  const char* args = (end && *end == ',') ? end + 1 : "";
  uint8_t result = command <= 65535 ? handleOvmsV2Command((uint16_t)command, args) : 2;
  char response[96];
  snprintf(response, sizeof(response), "MP-0 c%lu,%u", command, result);
  transmit(response);
}

void processEncryptedLine(const String& encoded) {
  uint8_t* decoded = nullptr;
  size_t len = 0;
  if (!base64Decode(encoded, decoded, len)) { resetConnection(); return; }
  rc4Crypt(rxRc4, decoded, len);
  String message((char*)decoded, len);
  delete[] decoded;
  if (!message.startsWith("MP-0 ") || message.length() < 6) { resetConnection(); return; }
  if (message.startsWith("MP-0 EM") && paranoidReady && message.length() >= 8) {
    char code = message[7];
    uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    if (!base64Decode(message.substring(8), payload, payloadLen)) { resetConnection(); return; }
    Rc4State paranoidRc4;
    rc4Init(paranoidRc4, paranoidDigest, sizeof(paranoidDigest));
    uint8_t discard[1024] = {};
    rc4Crypt(paranoidRc4, discard, sizeof(discard));
    rc4Crypt(paranoidRc4, payload, payloadLen);
    message = "MP-0 ";
    message += code;
    for (size_t i = 0; i < payloadLen; i++) message += (char)payload[i];
    delete[] payload;
  }
  char code = message[5];
  if (code == 'A') transmit("MP-0 a");
  else if (code == 'C') processCommand(message.c_str() + 6);
}

void processLogin(const String& line) {
  if (!line.startsWith("MP-S 0 ")) { resetConnection(); return; }
  int separator = line.indexOf(' ', 7);
  if (separator < 0) { resetConnection(); return; }
  String serverToken = line.substring(7, separator);
  String serverDigest = line.substring(separator + 1);
  if (serverToken == clientToken) { resetConnection(); return; }
  uint8_t digest[16];
  if (!hmacMd5(serverToken, getOvmsPassword(), digest) || base64Encode(digest, sizeof(digest)) != serverDigest) {
    Serial.println("[ovms-v2] server authentication failed");
    resetConnection();
    return;
  }
  String key = serverToken + clientToken;
  if (!hmacMd5(key, getOvmsPassword(), digest)) { resetConnection(); return; }
  rc4Init(txRc4, digest, sizeof(digest));
  rc4Init(rxRc4, digest, sizeof(digest));
  uint8_t discard[1024] = {};
  rc4Crypt(txRc4, discard, sizeof(discard));
  memset(discard, 0, sizeof(discard));
  rc4Crypt(rxRc4, discard, sizeof(discard));
  authenticated = true;
  lastReceive = millis();
  // Keep the ping away from the 60-second telemetry snapshot phase.
  lastPing = millis() - 15000;
  Serial.println("[ovms-v2] authenticated");
  if (strlen(getOvmsModulePassword()) > 0) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    paranoidToken = "";
    for (uint8_t i = 0; i < 22; i++) paranoidToken += alphabet[esp_random() % 64];
    if (!hmacMd5(paranoidToken, getOvmsModulePassword(), paranoidDigest)) {
      resetConnection();
      return;
    }
    if (!transmitTransport("MP-0 ET" + paranoidToken)) {
      resetConnection();
      return;
    }
    paranoidReady = true;
    Serial.println("[ovms-v2] paranoid mode enabled");
  }
  startSnapshot();
}

bool connectServer() {
  if (!strlen(getOvmsServer()) || !strlen(getVehicleId()) || !strlen(getOvmsPassword())) return false;
  if (WiFi.status() == WL_CONNECTED) {
    if (getOvmsTls()) {
      if (ESP.getFreeHeap() < 90000) {
        Serial.printf("[ovms-v2] TLS deferred, low heap: %lu\n", (unsigned long)ESP.getFreeHeap());
        return false;
      }
      tlsClient = new (std::nothrow) WiFiClientSecure();
      if (!tlsClient) return false;
      tlsClient->setInsecure();
      tlsClient->setTimeout(1);
      tlsClient->setHandshakeTimeout(8);
      client = tlsClient;
    } else {
      plainClient.setConnectionTimeout(3000);
      client = &plainClient;
    }
  } else if (!getOvmsTls() && modemMqttReady()) {
    client = modemTcpClient();
  } else return false;
  if (!client || !client->connect(getOvmsServer(), getOvmsPort())) { resetConnection(); return false; }
  connectedAt = millis();

  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  clientToken = "";
  for (uint8_t i = 0; i < 22; i++) clientToken += alphabet[esp_random() % 64];
  uint8_t digest[16];
  if (!hmacMd5(clientToken, getOvmsPassword(), digest)) { resetConnection(); return false; }
  String login = "MP-C 0 " + clientToken + " " + base64Encode(digest, sizeof(digest)) + " " + getVehicleId() + "\r\n";
  client->print(login);
  Serial.println("[ovms-v2] login sent");
  return true;
}
} // namespace

void setupOvmsV2() {
  plainClient.setTimeout(1);
  rxLine.reserve(512);
}

void manageOvmsV2() {
  if (getRemoteTransport() != RemoteTransport::OVMS_V2) { resetConnection(); return; }
  if (!client || !client->connected()) {
    resetConnection();
    if (lastConnectAttempt == 0 || millis() - lastConnectAttempt >= 60000) {
      lastConnectAttempt = millis();
      connectServer();
    }
    return;
  }
  while (client->available()) {
    char c = (char)client->read();
    lastReceive = millis();
    if (c == '\r') continue;
    if (c == '\n') {
      if (rxLine.length()) {
        if (authenticated) processEncryptedLine(rxLine); else processLogin(rxLine);
        rxLine = "";
        if (!client) return;
      }
    } else if (rxLine.length() < 1024) rxLine += c;
    else {
      resetConnection();
      return;
    }
  }
  if (!authenticated && connectedAt != 0 && millis() - connectedAt > 15000) {
    Serial.println("[ovms-v2] authentication timeout");
    resetConnection();
    return;
  }
  if (authenticated && snapshotPart < 5 && (long)(millis() - nextSnapshotPartAt) >= 0) {
    if (!sendNextSnapshotPart()) {
      resetConnection();
      return;
    }
  }
  if (authenticated && snapshotPart >= 5 && nextSnapshotAt != 0 &&
      (long)(millis() - nextSnapshotAt) >= 0) {
    startSnapshot();
  }
  if (authenticated && millis() - lastPing >= 60000) {
    if (!transmit("MP-0 A")) {
      resetConnection();
      return;
    }
    lastPing = millis();
  }
}

void ovmsV2Disconnect() { resetConnection(); }
bool ovmsV2IsConnected() { return authenticated && client && client->connected(); }
bool ovmsV2ParanoidIsActive() { return ovmsV2IsConnected() && paranoidReady; }
