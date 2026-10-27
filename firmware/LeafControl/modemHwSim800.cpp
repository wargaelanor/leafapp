#include "modemHw.h"
#include "callAudio.h"

#include <Client.h>

namespace {

HardwareSerial& simSerial = Serial2;
bool passthroughActive = false;
String savedApn;
String savedUser;
String savedPass;

void flushInput() {
  while (simSerial.available()) simSerial.read();
}

void sendCommand(const String& command) {
  flushInput();
  simSerial.print(command);
  simSerial.print("\r\n");
}

bool awaitText(const char* expected, uint32_t timeoutMs, String* response = nullptr) {
  String data;
  unsigned long started = millis();
  while (millis() - started < timeoutMs) {
    int guard = 256;
    while (simSerial.available() && guard-- > 0) {
      data += (char)simSerial.read();
      if (data.length() > 1024) data.remove(0, 512);
      if (data.indexOf(expected) >= 0) {
        if (response) *response = data;
        return true;
      }
    }
    delay(1);
  }
  if (response) *response = data;
  return false;
}

bool commandOk(const String& command, uint32_t timeoutMs = 2000, String* response = nullptr) {
  sendCommand(command);
  String data;
  bool ok = awaitText("OK", timeoutMs, &data);
  if (command == "AT" && data.length() > 0) {
    String printable = data;
    printable.replace("\r", "<CR>");
    printable.replace("\n", "<LF>");
    Serial.printf("[modem] raw AT response: [%s]\n", printable.c_str());
  }
  if (response) *response = data;
  return ok;
}

bool fileHasSize(const String& path, size_t expected) {
  String response;
  if (!commandOk("AT+FSFLSIZE=" + path, 2500, &response)) return false;
  int marker = response.indexOf("+FSFLSIZE:");
  if (marker < 0) return false;
  return (size_t)response.substring(marker + 11).toInt() == expected;
}

bool uploadFile(const String& path, const uint8_t* data, size_t length) {
  commandOk("AT+FSDEL=" + path, 2500);
  if (!commandOk("AT+FSCREATE=" + path, 2500)) return false;
  sendCommand("AT+FSWRITE=" + path + ",0," + String(length) + ",15");
  if (!awaitText(">", 3000)) return false;
  simSerial.write(data, length);
  simSerial.flush();
  return awaitText("OK", 18000) && fileHasSize(path, length);
}

bool ensureVoiceFile(const String& path, const uint8_t* data, size_t length) {
  if (fileHasSize(path, length)) return true;
  Serial.printf("[call] uploading %s (%u bytes)\n", path.c_str(), (unsigned)length);
  bool ok = uploadFile(path, data, length);
  Serial.printf("[call] upload %s\n", ok ? "complete" : "failed");
  return ok;
}

bool escapeToAt() {
  flushInput();
  unsigned long started = millis();
  while (millis() - started < 1200) {
    while (simSerial.available()) simSerial.read();
    delay(1);
  }
  simSerial.print("+++");
  bool ok = awaitText("OK", 2000);
  passthroughActive = false;
  return ok;
}

bool parseBearerIp(const String& response, String& ip) {
  int marker = response.indexOf("+SAPBR:");
  if (marker < 0) return false;
  int firstQuote = response.indexOf('"', marker);
  int secondQuote = response.indexOf('"', firstQuote + 1);
  if (firstQuote < 0 || secondQuote <= firstQuote) return false;
  ip = response.substring(firstQuote + 1, secondQuote);
  return ip.length() > 0 && ip != "0.0.0.0";
}

bool bringUpGprs(String& ip) {
  String response;
  if (commandOk("AT+SAPBR=2,1", 4000, &response) && parseBearerIp(response, ip)) return true;

  commandOk("AT+CIPSHUT", 10000);
  commandOk("AT+CIPMODE=1", 2000);
  commandOk("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", 2000);
  commandOk("AT+CSTT=\"" + savedApn + "\",\"" + savedUser + "\",\"" + savedPass + "\"", 2500);
  commandOk("AT+SAPBR=1,1", 45000);
  commandOk("AT+CIICR", 45000);

  for (uint8_t attempt = 0; attempt < 5; attempt++) {
    if (commandOk("AT+SAPBR=2,1", 8000, &response) && parseBearerIp(response, ip)) return true;
    delay(3000);
  }
  return false;
}

String resolveHost(const char* host) {
  IPAddress parsed;
  if (parsed.fromString(host)) return String(host);

  // The SIM800 DNS client is unreliable on some Beeline PDP sessions.
  // Keep the broker reachable without depending on CDNSGIP.
  if (String(host) == "m5.wqtt.ru") return "45.141.211.73";

  commandOk("AT+CDNSCFG=\"8.8.8.8\",\"8.8.4.4\"", 2000);
  sendCommand("AT+CDNSGIP=\"" + String(host) + "\"");
  String response;
  if (!awaitText("OK", 20000, &response)) {
    Serial.printf("[modem] DNS timeout for %s\n", host);
    return String(host);
  }
  int marker = response.indexOf("+CDNSGIP:");
  if (marker < 0) return String(host);
  int cursor = marker;
  while (true) {
    int first = response.indexOf('"', cursor);
    int second = response.indexOf('"', first + 1);
    if (first < 0 || second < 0) break;
    String token = response.substring(first + 1, second);
    IPAddress ip;
    if (ip.fromString(token)) return token;
    cursor = second + 1;
  }
  Serial.printf("[modem] DNS did not return an IP for %s\n", host);
  return String(host);
}

class Sim800Client : public Client {
 public:
  int connect(IPAddress ip, uint16_t port) override {
    String host = ip.toString();
    return connect(host.c_str(), port);
  }

  int connect(const char* host, uint16_t port) override {
    if (passthroughActive) return 1;
    if (!savedApn.length()) return 0;

    String ip;
    if (!bringUpGprs(ip)) return 0;
    // SIM800 can resolve the broker name as part of CIPSTART. A separate
    // CDNSGIP request is unreliable on some firmware revisions.
    String target = resolveHost(host);
    Serial.printf("[modem] TCP start %s:%u target=%s\n", host, port, target.c_str());
    // Clear a socket left by a previous MQTT attempt before entering
    // transparent TCP mode again. The bearer remains active.
    sendCommand("AT+CIPSHUT");
    awaitText("OK", 10000);
    commandOk("AT+CIPMODE=1", 3000);
    sendCommand("AT+CIPSTART=\"TCP\",\"" + target + "\"," + String(port));

    String line;
    String raw;
    unsigned long started = millis();
    while (millis() - started < 40000) {
      while (simSerial.available()) {
        char c = (char)simSerial.read();
        raw += c;
        if (raw.length() > 512) raw.remove(0, 256);
        if (c == '\r') continue;
        if (c == '\n') {
          line.trim();
          if (line == "CONNECT" || line == "CONNECT OK") {
            delay(150);
            flushInput();
            passthroughActive = true;
            Serial.printf("[modem] TCP passthrough %s:%u connected\n", host, port);
            return 1;
          }
          if (line == "CONNECT FAIL" || line == "ERROR" || line == "CLOSED") {
            Serial.printf("[modem] TCP connect failed: %s\n", line.c_str());
            return 0;
          }
          line = "";
        } else {
          line += c;
        }
      }
      delay(2);
    }
    raw.replace("\r", " ");
    raw.replace("\n", " ");
    Serial.printf("[modem] TCP connect timeout, last line: %s raw: [%s]\n", line.c_str(), raw.c_str());
    return 0;
  }

  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    return passthroughActive ? simSerial.write(buffer, size) : 0;
  }
  int available() override { return passthroughActive ? simSerial.available() : 0; }
  int read() override { return available() ? simSerial.read() : -1; }
  int read(uint8_t* buffer, size_t size) override {
    return passthroughActive ? simSerial.read(buffer, size) : -1;
  }
  int peek() override { return available() ? simSerial.peek() : -1; }
  void flush() override { simSerial.flush(); }
  void stop() override {
    if (passthroughActive) escapeToAt();
    commandOk("AT+CIPSHUT", 10000);
    passthroughActive = false;
  }
  uint8_t connected() override { return passthroughActive ? 1 : 0; }
  operator bool() override { return connected(); }
};

Sim800Client tcpClient;

}

namespace modem800 {

void hwBegin() {
  // ESP RX <- SIM800 TXD, ESP TX -> SIM800 RXD.
  simSerial.begin(115200, SERIAL_8N1, 35, 38);
  delay(100);
  flushInput();
  escapeToAt();
  commandOk("ATQ0V1", 1500);
  commandOk("AT+IFC=0,0", 1500);
  commandOk("AT&D0", 1500);
  commandOk("AT+CSCLK=0", 1500);
  commandOk("ATE0", 1500);
  commandOk("AT+CMEE=1", 1000);
}

bool hwTestAt(uint32_t timeoutMs) { return commandOk("AT", timeoutMs); }

bool hwSimReady() {
  String response;
  return commandOk("AT+CPIN?", 3000, &response) && response.indexOf("READY") >= 0;
}

bool hwSelectNetwork(const char* apn, const char* user) {
  String apnValue = apn ? apn : "";
  String userValue = user ? user : "";
  apnValue.toLowerCase();
  userValue.toLowerCase();
  if (apnValue.indexOf("beeline") >= 0 || userValue == "beeline") {
    Serial.println("[modem] selecting Beeline (25099)");
    return commandOk("AT+COPS=1,2,\"25099\"", 180000);
  }
  commandOk("AT+COPS=0", 10000);
  return true;
}

bool hwNetworkConnected() {
  String response;
  if (!commandOk("AT+CREG?", 3000, &response)) return false;
  return response.indexOf(",1") >= 0 || response.indexOf(",5") >= 0;
}

int16_t hwSignal() {
  String response;
  if (!commandOk("AT+CSQ", 2000, &response)) return -1;
  int marker = response.indexOf("+CSQ:");
  if (marker < 0) return -1;
  int comma = response.indexOf(',', marker);
  return response.substring(marker + 5, comma).toInt();
}

bool hwGprsConnect(const char* apn, const char* user, const char* pass) {
  savedApn = apn ? apn : "";
  savedUser = user ? user : "";
  savedPass = pass ? pass : "";
  String ip;
  bool ok = bringUpGprs(ip);
  Serial.printf("[modem] GPRS %s%s%s\n", ok ? "IP " : "failed", ok ? ip.c_str() : "", ok ? "" : "");
  return ok;
}

bool hwGprsDisconnect() {
  tcpClient.stop();
  savedApn = "";
  savedUser = "";
  savedPass = "";
  return true;
}

Client* hwTcpClient() { return &tcpClient; }

bool hwPrepareVoiceCalls() {
  if (passthroughActive) tcpClient.stop();
  bool clip = commandOk("AT+CLIP=1", 2000);
  String response;
  bool dtam = commandOk("AT+DTAM=?", 2000, &response) && response.indexOf("+DTAM:") >= 0;
  bool crec = commandOk("AT+CREC=?", 2000, &response) && response.indexOf("+CREC:") >= 0;
  bool files = false;
  if (clip && dtam && crec) {
    commandOk("AT+FSHEX=0", 1500);
    commandOk("AT+CREC=9,0", 3000); // Ensure C:\User record directory exists.
    files = ensureVoiceFile("C:\\User\\climate_on.amr", CLIMATE_ON_AMR, CLIMATE_ON_AMR_LEN) &&
            ensureVoiceFile("C:\\User\\climate_off.amr", CLIMATE_OFF_AMR, CLIMATE_OFF_AMR_LEN);
  }
  Serial.printf("[call] SIM800 voice support CLIP=%d DTAM=%d CREC=%d files=%d\n",
                clip, dtam, crec, files);
  return clip && dtam && crec && files;
}

bool hwIncomingCaller(String* number, uint32_t timeoutMs) {
  if (passthroughActive) tcpClient.stop();
  commandOk("AT+CLIP=1", 2000);
  unsigned long started = millis();
  while (millis() - started < timeoutMs) {
    String response;
    if (commandOk("AT+CLCC", 2000, &response)) {
      int marker = response.indexOf("+CLCC:");
      int firstQuote = response.indexOf('"', marker);
      int secondQuote = response.indexOf('"', firstQuote + 1);
      if (marker >= 0 && firstQuote > marker && secondQuote > firstQuote) {
        if (number) *number = response.substring(firstQuote + 1, secondQuote);
        return true;
      }
    }
    delay(350);
  }
  return false;
}

void hwRejectCall() {
  commandOk("ATH", 3000);
}

bool hwAnswerAndPlay(uint8_t recordId) {
  String path = recordId == 1 ? "C:\\User\\climate_on.amr" : "C:\\User\\climate_off.amr";
  size_t expected = recordId == 1 ? CLIMATE_ON_AMR_LEN : CLIMATE_OFF_AMR_LEN;
  String response;
  if (!fileHasSize(path, expected)) {
    Serial.printf("[call] voice file %s is missing\n", path.c_str());
    hwRejectCall();
    return false;
  }
  if (!commandOk("ATA", 10000)) {
    Serial.println("[call] ATA failed");
    return false;
  }
  delay(500);
  if (!commandOk("AT+DTAM=1", 2000)) {
    Serial.println("[call] remote audio mode unsupported");
    hwRejectCall();
    return false;
  }
  if (!commandOk("AT+CREC=4,\"" + path + "\",0,80,0", 3000)) {
    Serial.println("[call] voice playback failed");
    hwRejectCall();
    return false;
  }

  // Wait for the recording to finish, with a hard cap so a corrupt slot
  // cannot hold the call indefinitely.
  unsigned long started = millis();
  delay(500);
  while (millis() - started < 12000) {
    if (commandOk("AT+CREC?", 2000, &response) && response.indexOf("+CREC: 0") >= 0) break;
    delay(300);
  }
  hwRejectCall();
  return true;
}

bool hwEnableGps() { return false; }
bool hwGetGps(float*, float*, float*, float*) { return false; }

void hwPulsePwrKey(int pin, bool powerOn) {
  if (pin <= 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(powerOn ? 1200 : 1800);
  digitalWrite(pin, HIGH);
}

}
