#include "modemProbe.h"

#include <HardwareSerial.h>
#include <WiFi.h>

#include "configuration.h"
#include "modemHw.h"
#include "modemManager.h"
#include "modemPins.h"
#include "serverApi.h"

// Полная диагностика SIM800: UART -> SIM -> сеть -> GPRS -> реальный TCP до
// сервера. Менеджер модема на время проверки останавливается, иначе два
// владельца Serial2 мешают друг другу и AT-обмен рассыпается.

static HardwareSerial& probeSerial = Serial2;
static volatile bool probeRunning = false;
static String probeResult;

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
      if (out.length() < 1500) out += c;
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
  reply = readFor(700);
  return reply.indexOf("OK") >= 0;
}

// Отправляет команду и возвращает ответ (+OK / +ERROR / URC).
static String ask(const char* cmd, uint32_t ms, String& log, bool quiet = false) {
  flushProbe();
  probeSerial.print(cmd);
  probeSerial.print("\r\n");
  String reply = readFor(ms);
  if (!quiet) {
    log += "> ";
    log += cmd;
    log += "\n";
    log += reply;
    if (!reply.endsWith("\n")) log += "\n";
  }
  return reply;
}

static long valueAfter(const String& reply, const char* marker, long fallback = -1) {
  int i = reply.indexOf(marker);
  if (i < 0) return fallback;
  i += strlen(marker);
  while (i < (int)reply.length() && (reply[i] == ' ' || reply[i] == ':')) i++;
  long v = 0;
  bool any = false;
  while (i < (int)reply.length() && reply[i] >= '0' && reply[i] <= '9') {
    v = v * 10 + (reply[i] - '0');
    any = true;
    i++;
  }
  return any ? v : fallback;
}


// Второе число в ответе ("+CREG: 0,1" -> 1). Первое — режим уведомлений.
static long secondValue(const String& reply, const char* marker, long fallback = -1) {
  int i = reply.indexOf(marker);
  if (i < 0) return fallback;
  int comma = reply.indexOf(',', i);
  if (comma < 0) return valueAfter(reply, marker, fallback);
  int j = comma + 1;
  while (j < (int)reply.length() && reply[j] == ' ') j++;
  long v = 0;
  bool any = false;
  while (j < (int)reply.length() && reply[j] >= '0' && reply[j] <= '9') {
    v = v * 10 + (reply[j] - '0');
    any = true;
    j++;
  }
  return any ? v : fallback;
}

static String quoted(const String& reply) {
  int a = reply.indexOf('"');
  if (a < 0) return String();
  int b = reply.indexOf('"', a + 1);
  if (b <= a) return String();
  return reply.substring(a + 1, b);
}

static String operatorName(const String& cops) {
  int a = cops.indexOf('"');
  if (a < 0) return String();
  int b = cops.indexOf('"', a + 1);
  if (b <= a) return String();
  return cops.substring(a + 1, b);
}

String modemProbeJson() {
  if (probeRunning) {
    return String("{\"running\":true,\"ok\":false,\"reply\":\"проверка идёт…\"}");
  }
  return probeResult.length() ? probeResult
                              : String("{\"ok\":false,\"reply\":\"проверка ещё не запускалась\"}");
}

// Сама проверка. Выполняется в отдельной задаче: GPRS может занять минуту.
static String modemProbeRun() {
  // Забираем UART у менеджера модема.
  modemManagerPause(true);
  unsigned long waitStart = millis();
  while (modemTaskBusy() && millis() - waitStart < 5000) delay(50);

  // Питание и служебные линии модуля: на T-Call без этого SIM800 не включён.
  modem800::hwBegin();

  String log;
  log.reserve(1800);
  log += "Пины UART: RX=";
  log += String(MODEM_UART_RX);
  log += " TX=";
  log += String(MODEM_UART_TX);
  log += ", питание модуля: ";
  log += getModemEnable() ? "включено в настройках" : "выключено в настройках";
  log += "\n\n";

  static const uint32_t kBauds[] = {115200, 9600, 38400, 57600, 230400, 460800};
  uint32_t found = 0;
  String reply;
  for (size_t i = 0; i < sizeof(kBauds) / sizeof(kBauds[0]); i++) {
    if (tryAt(kBauds[i], reply)) {
      found = kBauds[i];
      break;
    }
  }

  // Нет ответа: пробуем разбудить модуль импульсом PWRKEY (если пин известен).
  if (!found) {
    int pwrKey = modemProbePwrKeyPin();
    if (pwrKey > 0) {
      log += "Нет ответа на AT. Импульс PWRKEY (GPIO";
      log += String(pwrKey);
      log += "), ждём 8 с...\n";
      modemProbePowerPulse();
      delay(8000);
      for (size_t i = 0; i < 2; i++) {
        if (tryAt(kBauds[i], reply)) {
          found = kBauds[i];
          break;
        }
      }
    } else {
      log += "Нет ответа на AT. Пин PWRKEY у этой платы не подключён — модуль должен\n";
      log += "включаться сам от питания.\n";
    }
  }

  if (!found) {
    log += "\nИтог: модем не отвечает на AT ни на одной скорости.\n";
    log += "Проверьте: питание модуля (3.4-4.2 В, до 2 А), общий GND, TX модуля -> RX платы\n";
    log += "и RX модуля -> TX платы, уровень 3.3 В, SIM вставлена до щелчка.\n";
    modemManagerPause(false);
    return String("{\"ok\":false,\"stage\":\"uart\",\"baud\":0,\"verdict\":\"модем не отвечает на AT\",\"reply\":\"") +
           jsonEscape(log) + "\"}";
  }

  log += "AT OK на ";
  log += String(found);
  log += " бод\n\n";

  // SIM800 отвечает медленно, а echo выключен: слишком короткий таймаут
  // обрезал ответ (CPIN приходил как "Y") и диагностика врала.
  String ati = ask("ATI", 1500, log);
  String cpin = ask("AT+CPIN?", 2500, log);
  String csq = ask("AT+CSQ", 1500, log);
  String creg = ask("AT+CREG?", 2000, log);
  String cops = ask("AT+COPS?", 2000, log);
  String cgatt = ask("AT+CGATT?", 2000, log);
  String imei = ask("AT+GSN", 2000, log);
  ask("AT+CGDCONT?", 2000, log);

  bool simOk = cpin.indexOf("READY") >= 0;
  long signal = valueAfter(csq, "+CSQ:", -1);
  long reg = secondValue(creg, "+CREG:", -1);
  bool registered = (reg == 1 || reg == 5);
  bool attached = valueAfter(cgatt, "+CGATT:", 0) == 1;

  String model;
  {
    int pos = 0;
    while (pos < (int)ati.length()) {
      int nl = ati.indexOf('\n', pos);
      String line = (nl < 0) ? ati.substring(pos) : ati.substring(pos, nl);
      line.trim();
      if (line.length() && !line.startsWith("AT") && line != "OK") {
        model = line;
        break;
      }
      if (nl < 0) break;
      pos = nl + 1;
    }
  }

  String op = operatorName(cops);

  // ---- GPRS и настоящий интернет
  String ip;
  bool tcpOk = false;
  bool gprsOk = false;
  String apn = getModem800Apn();

  log += "\n--- GPRS (APN: ";
  log += apn.length() ? apn : "не задан";
  log += ") ---\n";

  if (apn.length() == 0) {
    log += "APN пустой: укажите APN оператора в настройках модема.\n";
  } else if (!simOk) {
    log += "SIM не готова — GPRS не поднимаем.\n";
  } else {
    ask("AT+CIPSHUT", 8000, log);
    ask(("AT+CSTT=\"" + apn + "\",\"" + String(getModem800User()) + "\",\"" +
         String(getModem800Pass()) + "\"").c_str(), 3000, log);
    ask("AT+CIICR", 20000, log);
    String cifsr = ask("AT+CIFSR", 3000, log);
    int dot = cifsr.indexOf('.');
    if (dot > 0) {
      int lineStart = cifsr.lastIndexOf('\n', dot);
      ip = cifsr.substring(lineStart < 0 ? 0 : lineStart + 1);
      int nl2 = ip.indexOf('\n');
      if (nl2 > 0) ip = ip.substring(0, nl2);
      ip.trim();
      ip.replace("\r", "");
    }
    for (uint8_t i = 0; i < 5 && ip.length() == 0; i++) {
      delay(2000);
      String again = ask("AT+CIFSR", 2000, log, true);
      if (again.indexOf('.') >= 0) {
        ip = again;
        ip.replace("\r", "");
        int a = ip.indexOf('.');
        int ls = ip.lastIndexOf('\n', a);
        int le = ip.indexOf('\n', a);
        ip = ip.substring(ls < 0 ? 0 : ls + 1, le < 0 ? ip.length() : le);
        ip.trim();
      }
    }
    gprsOk = ip.length() > 0 && ip != "0.0.0.0";

    if (gprsOk) {
      log += "\n--- проверка HTTP (AT+HTTP, httpbin.org) ---\n";
  {
    String httpResp;
    int httpCode = 0;
    bool httpOk = modem800::hwHttp(0, "http://httpbin.org/get", nullptr, httpResp, httpCode);
    log += String("HTTP ") + (httpOk ? "ok" : "fail") + ", код " + String(httpCode);
    if (httpResp.length()) {
      String brief = httpResp.substring(0, 120);
      brief.replace("\r", " ");
      brief.replace("\n", " ");
      log += ": ";
      log += brief;
    }
    log += "\n";
  }

  log += "\n--- проверка интернета (TCP до сервера:80) ---\n";
      String host = "leaf.wargaelanor.keenetic.link";
      String start = ask(("AT+CIPSTART=\"TCP\",\"" + host + "\",80").c_str(), 12000, log);
      tcpOk = start.indexOf("CONNECT OK") >= 0 || start.indexOf("ALREADY CONNECT") >= 0;
      if (tcpOk) {
        ask("AT+CIPCLOSE", 4000, log);
      }
    }
    ask("AT+CIPSHUT", 8000, log, true);
  }

  String verdict;
  verdict += "Модуль: ";
  verdict += model.length() ? model : "отвечает";
  verdict += "\nSIM: ";
  verdict += simOk ? "OK" : "не готова (проверьте PIN/контакты)";
  verdict += "\nСеть: ";
  if (registered) {
    verdict += op.length() ? op : "зарегистрирован";
  } else {
    verdict += "не зарегистрирован (CREG=";
    verdict += String(reg);
    verdict += ")";
  }
  verdict += "\nСигнал: ";
  if (signal >= 0 && signal != 99) {
    verdict += String(signal);
    verdict += "/31 (";
    verdict += String(-113 + 2 * signal);
    verdict += " dBm)";
  } else {
    verdict += "нет данных";
  }
  verdict += "\nGPRS: ";
  verdict += gprsOk ? ("IP " + ip) : "нет PDP-контекста";
  verdict += "\nИнтернет: ";
  verdict += tcpOk ? "TCP до сервера установлен"
                   : (gprsOk ? "TCP до сервера не прошёл" : "не проверялся");
  verdict += "\n";
  if (WiFi.status() == WL_CONNECTED) {
    unsigned long ok = serverApiLastOkMs();
    verdict += "Wi-Fi: подключён, RSSI ";
    verdict += String(WiFi.RSSI());
    verdict += " dBm, последний обмен с сервером ";
    verdict += ok ? (String((millis() - ok) / 1000UL) + " с назад") : String("не было");
    verdict += "\n";
  } else {
    verdict += "Wi-Fi: не подключён\n";
  }

  modemManagerPause(false);

  String out = "{\"ok\":true,\"baud\":";
  out += String(found);
  out += ",\"sim\":";
  out += simOk ? "true" : "false";
  out += ",\"registered\":";
  out += registered ? "true" : "false";
  out += ",\"signal\":";
  out += String(signal);
  out += ",\"attached\":";
  out += attached ? "true" : "false";
  out += ",\"gprs\":";
  out += gprsOk ? "true" : "false";
  out += ",\"ip\":\"";
  out += jsonEscape(ip);
  out += "\",\"tcp\":";
  out += tcpOk ? "true" : "false";
  out += ",\"verdict\":\"";
  out += jsonEscape(verdict);
  out += "\",\"reply\":\"";
  out += jsonEscape(verdict + "\n--- полный лог AT ---\n" + log);
  out += "\"}";
  return out;
}

int modemProbePwrKeyPin() {
  int pin = getModemPwrKey();
#if defined(BOARD_MODEM_PWRKEY) && (BOARD_MODEM_PWRKEY > 0)
  if (pin <= 0) pin = BOARD_MODEM_PWRKEY;
#endif
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


// ---------------------------------------------------------------- задача

static void probeTask(void*) {
  String result = modemProbeRun();
  probeResult = result;
  probeRunning = false;    // флаг снимаем последним: результат уже готов
  vTaskDelete(NULL);
}

void modemProbeStart() {
  if (probeRunning) return;
  probeRunning = true;
  if (xTaskCreatePinnedToCore(probeTask, "mdm-probe", 8192, nullptr, 0, nullptr, 0) != pdPASS) {
    probeRunning = false;
    probeResult = "{\"ok\":false,\"reply\":\"не удалось запустить задачу проверки\"}";
  }
}

bool modemProbeActive() { return probeRunning; }
