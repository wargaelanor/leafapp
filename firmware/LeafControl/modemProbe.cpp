#include "modemProbe.h"
#include <HardwareSerial.h>
#include "modemPins.h"
#include "configuration.h"

// The modem manager owns Serial2 while it is active. The probe re-opens it at
// each candidate baud rate; only run it when the modem is disabled or failed.
static HardwareSerial& probeSerial = Serial2;

static String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\r': out += "\\r"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) out += ' ';
        else out += c;
    }
  }
  return out;
}

static String readFor(uint32_t ms) {
  String out;
  unsigned long start = millis();
  while (millis() - start < ms) {
    while (probeSerial.available()) {
      char c = (char)probeSerial.read();
      if (out.length() < 1200) out += c;
    }
    delay(1);
  }
  return out;
}

static void flushProbe() {
  while (probeSerial.available()) probeSerial.read();
}

static bool tryAt(uint32_t baud, String& reply) {
  probeSerial.end();
  probeSerial.begin(baud, SERIAL_8N1, MODEM_UART_RX, MODEM_UART_TX);
  delay(60);
  flushProbe();
  probeSerial.print("AT\r\n");
  reply = readFor(600);
  return reply.indexOf("OK") >= 0;
}

static void ask(const char* cmd, String& out) {
  flushProbe();
  probeSerial.print(cmd);
  probeSerial.print("\r\n");
  out += "> ";
  out += cmd;
  out += "\n";
  out += readFor(900);
  out += "\n";
}

String modemProbeJson() {
  static const uint32_t kBaubds[] = {115200, 9600, 38400, 57600, 230400, 460800};
  uint32_t found = 0;
  String reply;
  for (size_t i = 0; i < sizeof(kBaubds) / sizeof(kBaubds[0]); i++) {
    if (tryAt(kBaubds[i], reply)) {
      found = kBaubds[i];
      break;
    }
  }

  if (!found) {
    String out = "нет ответа ни на одной скорости: 115200, 9600, 38400, 57600, 230400, 460800\n";
    out += "Проверьте питание модуля (3.4-4.2 В, до 2 А), импульс PWRKEY, пересечение TX/RX и общий GND.";
    return String("{\"ok\":false,\"baud\":0,\"reply\":\"") + jsonEscape(out) + "\"}";
  }

  String out;
  out.reserve(1200);
  out += "AT OK на ";
  out += String(found);
  out += " бод\n\n";
  ask("ATI", out);
  ask("AT+CPIN?", out);
  ask("AT+CSQ", out);
  ask("AT+CREG?", out);
  ask("AT+COPS?", out);
  ask("AT+CGATT?", out);
  ask("AT+IPR?", out);

  return String("{\"ok\":true,\"baud\":") + String(found) +
         ",\"reply\":\"" + jsonEscape(out) + "\"}";
}

int modemProbePwrKeyPin() {
  int pin = getModemPwrKey();
  return pin > 0 ? pin : 0;
}

void modemProbePowerPulse() {
  int pin = modemProbePwrKeyPin();
  if (pin <= 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  delay(120);
  // SIMCom: PWRKEY low for ~1 s switches the module on (or off).
  digitalWrite(pin, LOW);
  delay(1200);
  digitalWrite(pin, HIGH);
}
