// A7670E bench test for ESP32-C3 Mini (native USB, COM8).
//
// Wiring (A7670 mini core board -> ESP32-C3):
//   4GTX  (module TX)  -> GPIO4  (ESP RX)
//   4GRX  (module RX)  -> GPIO5  (ESP TX)
//   PWREN              -> GPIO10
//   GND                -> GND
//   VIN                -> 3.8-4.2 V / 2 A (separate supply, NOT 3.3 V)
//
// The A7670 UART is a 1.8 V interface. The ESP32-C3 is 3.3 V, so use a level
// shifter (TXS0108E / BSS138 module) unless the carrier board already has one.
//
// Open Serial Monitor at 115200, line ending "Both NL & CR" or "Newline".
// Menu:
//   p - pulse PWREN (power on/off)
//   h - hold PWREN inactive (high if active low)
//   l - hold PWREN active (low if active low)
//   r - rescan baud rates
//   i - identity/SIM/network AT sequence
//   anything else is sent to the modem as an AT command (CRLF is added).

#include <Arduino.h>

#define MODEM_RX 4
#define MODEM_TX 5
#define MODEM_PWREN 10

// Most A7670 carrier boards switch the regulator with an active-low PWRKEY.
// If your board's PWREN is active-high, press 't' in the menu to invert it at
// runtime, or change this default to 0.
#define PWREN_ACTIVE_LOW 1

static bool pwrActiveLow = (PWREN_ACTIVE_LOW != 0);

static const uint32_t kBaunds[] = {115200, 9600, 38400, 57600, 230400, 460800, 921600};
static uint32_t modemBaud = 0;
static String usbLine;
static bool pinsSwapped = false;

static void pwrInactive() {
  digitalWrite(MODEM_PWREN, pwrActiveLow ? HIGH : LOW);
}

static void pwrActive() {
  digitalWrite(MODEM_PWREN, pwrActiveLow ? LOW : HIGH);
}

static String printable(const String& in, size_t maxLen = 160) {
  String out;
  for (size_t i = 0; i < in.length() && out.length() < maxLen; i++) {
    char c = in[i];
    if (c == '\r') out += "\\r";
    else if (c == '\n') out += "\\n";
    else if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) out += '.';
    else out += c;
  }
  return out;
}

static void pulsePwrKey(uint32_t ms) {
  Serial.printf("[test] PWREN active for %lu ms\n", (unsigned long)ms);
  pwrActive();
  delay(ms);
  pwrInactive();
  Serial.println("[test] PWREN released");
}

static String readModem(uint32_t ms) {
  String out;
  unsigned long start = millis();
  while (millis() - start < ms) {
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (out.length() < 1500) out += c;
    }
    delay(1);
  }
  return out;
}

static void openUart(uint32_t baud) {
  Serial1.end();
  int rx = pinsSwapped ? MODEM_TX : MODEM_RX;
  int tx = pinsSwapped ? MODEM_RX : MODEM_TX;
  Serial1.begin(baud, SERIAL_8N1, rx, tx);
  delay(50);
  while (Serial1.available()) Serial1.read();
}

static bool at(uint32_t baud, uint32_t waitMs, String* reply = nullptr) {
  openUart(baud);
  Serial1.print("AT\r\n");
  String r = readModem(waitMs);
  if (reply) *reply = r;
  return r.indexOf("OK") >= 0;
}

static uint32_t scanBaud() {
  for (int pass = 0; pass < 2; pass++) {
    pinsSwapped = (pass == 1);
    Serial.printf("[test] scanning baud rates (%s wiring)...\n",
                  pinsSwapped ? "swapped" : "normal");
    for (size_t i = 0; i < sizeof(kBaunds) / sizeof(kBaunds[0]); i++) {
      String r;
      Serial.printf("  %lu ... ", (unsigned long)kBaunds[i]);
      if (at(kBaunds[i], 700, &r)) {
        Serial.println("AT OK");
        if (pinsSwapped) {
          Serial.println("[test] WIRES ARE SWAPPED: move 4GTX to GPIO4 and 4GRX to GPIO5");
        }
        return kBaunds[i];
      }
      Serial.printf("no [%s]\n", printable(r, 70).c_str());
    }
  }
  pinsSwapped = false;
  return 0;
}

// Passive listen: the module may be booting at some baud and printing logs even
// when it does not answer AT yet. This shows what is actually on the wire.
static void sniffBauds() {
  Serial.println("[test] listening on each baud for 2.5 s (no AT)...");
  for (size_t i = 0; i < sizeof(kBaunds) / sizeof(kBaunds[0]); i++) {
    openUart(kBaunds[i]);
    String r = readModem(2500);
    Serial.printf("  %lu: %u bytes [%s]\n", (unsigned long)kBaunds[i],
                  (unsigned)r.length(), printable(r, 120).c_str());
    openUart(kBaunds[i]);
    Serial1.print("AT\r\n");
    String a = readModem(700);
    Serial.printf("        AT -> [%s]\n", printable(a, 80).c_str());
  }
}

static void sendCmd(const char* cmd, uint32_t waitMs = 1200) {
  if (!modemBaud) {
    Serial.println("[test] modem not detected, press r to scan");
    return;
  }
  openUart(modemBaud);
  Serial.printf("\n>> %s\n", cmd);
  Serial1.print(cmd);
  Serial1.print("\r\n");
  String r = readModem(waitMs);
  Serial.print(r);
}

static void identity() {
  sendCmd("ATI");
  sendCmd("AT+CGSN");       // IMEI
  sendCmd("AT+CPIN?");      // SIM
  sendCmd("AT+CSQ");        // signal
  sendCmd("AT+CREG?");
  sendCmd("AT+COPS?");
  sendCmd("AT+CGATT?");
  sendCmd("AT+IPR?");
  sendCmd("AT+CBC");        // supply voltage
}

static void printPwrState() {
  Serial.printf("[test] PWREN polarity: active-%s, pin=%d\n",
                pwrActiveLow ? "low" : "high", digitalRead(MODEM_PWREN));
}

static void menu() {
  Serial.println();
  Serial.println("=== A7670E test ===");
  Serial.println("p=PWREN pulse  h=pin HIGH  l=pin LOW  t=invert pulse polarity");
  Serial.println("r=rescan (auto normal/swapped)  x=swap RX/TX  s=sniff  i=info");
  Serial.println("type an AT command and press Enter");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== A7670E / ESP32-C3 test ===");
  Serial.printf("chip=%s flash=%lu\n", ESP.getChipModel(), (unsigned long)ESP.getFlashChipSize());

  pinMode(MODEM_PWREN, OUTPUT);
  pwrInactive();
  printPwrState();
  openUart(115200);

  // Do not pulse PWREN automatically: a pulse toggles an already-running
  // module off. Scan first; press 'p' if the module is off.
  Serial.println("[test] no automatic PWREN pulse; press p if the module is off");
  modemBaud = scanBaud();

  if (modemBaud) {
    Serial.printf("[test] modem online at %lu baud\n", (unsigned long)modemBaud);
    identity();
  } else {
    Serial.println("[test] no AT response.");
    Serial.println("[test] check: VIN 3.8-4.2V/2A, common GND, 4GTX->GPIO4, 4GRX->GPIO5,");
    Serial.println("[test] 1.8V level shifter, PWREN pulse, SIM inserted.");
  }
  menu();
}

void loop() {
  while (Serial1.available()) {
    Serial.write(Serial1.read());
  }

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      usbLine += c;
      if (usbLine.length() > 120) usbLine.remove(0, 20);
      continue;
    }
    String line = usbLine;
    usbLine = "";
    line.trim();
    if (!line.length()) continue;

    if (line == "p") {
      pulsePwrKey(1200);
      delay(12000);
      modemBaud = scanBaud();
      if (modemBaud) identity();
    } else if (line == "h") {
      digitalWrite(MODEM_PWREN, HIGH);
      Serial.println("[test] PWREN = HIGH");
    } else if (line == "l") {
      digitalWrite(MODEM_PWREN, LOW);
      Serial.println("[test] PWREN = LOW");
    } else if (line == "r") {
      modemBaud = scanBaud();
    } else if (line == "s") {
      sniffBauds();
    } else if (line == "t") {
      pwrActiveLow = !pwrActiveLow;
      printPwrState();
    } else if (line == "x") {
      pinsSwapped = !pinsSwapped;
      Serial.printf("[test] RX/TX wiring: %s\n", pinsSwapped ? "swapped" : "normal");
    } else if (line == "i") {
      identity();
    } else {
      sendCmd(line.c_str(), 1500);
    }
    menu();
  }
}
