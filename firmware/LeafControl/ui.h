#ifndef UI_H
#define UI_H

#include <Arduino.h>
#include <WebServer.h>
#include "vehicleTypes.h"
#include "configuration.h"

// Escape a string for use inside an HTML attribute value
static String htmlEscapeAttr(const char* s) {
  String out;
  for (const char* p = s; *p; p++) {
    switch (*p) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += *p; break;
    }
  }
  return out;
}

// sendContent("") terminates the chunked stream, so never send an empty String mid-page
static void sendNonEmpty(WebServer& server, const String& value) {
  if (value.length() > 0) {
    server.sendContent(value);
  }
}

// Board settings page — NissanConnect style
void sendSettingsPage(WebServer& server, bool mqttConnected, bool ovmsConnected, const char* saveResult) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent(F(
    "<!DOCTYPE html><html lang='ru'><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1, viewport-fit=cover'>"
    "<meta name='theme-color' content='#0077ac'><title>Настройки — Leaf Control</title>"
    "<style>\n"
    "*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}\n"
    "body{background:#f4f5f7;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;color:#333;display:flex;justify-content:center}\n"
    ".app{width:100%;max-width:480px;min-height:100vh;min-height:100dvh;background:#f4f5f7;display:flex;flex-direction:column}\n"
    ".appbar{height:48px;background:#f9f9f9;display:flex;align-items:center;padding:0 16px;flex-shrink:0;border-bottom:1px solid #f1f1f1}\n"
    ".appbar .brand{display:flex;align-items:center;gap:8px}\n"
    ".appbar .brand svg{width:28px;height:12px}\n"
    ".appbar .brand span{font-size:15px;color:#333;letter-spacing:.5px}\n"
    ".screens{flex:1;overflow-y:auto;padding-bottom:24px}\n"
    ".settings-list{margin:8px 16px;background:#fff;border-radius:8px;box-shadow:0 1px 3px rgba(0,0,0,.06);overflow:hidden}\n"
    ".section{background:#fff;border-bottom:1px solid #eceef1}\n"
    ".section:last-child{border-bottom:0}\n"
    ".section summary{list-style:none;min-height:54px;padding:0 16px;display:flex;align-items:center;justify-content:space-between;gap:12px;font-size:15px;font-weight:500;color:#333;cursor:pointer;user-select:none}\n"
    ".section summary::-webkit-details-marker{display:none}\n"
    ".section summary:after{content:'›';font-size:25px;font-weight:300;color:#8e8e93;line-height:1;transform:rotate(0);transition:transform .18s}\n"
    ".section[open] summary:after{transform:rotate(90deg)}\n"
    ".section[open] summary{color:#0077ac;border-bottom:1px solid #f0f1f3}\n"
    ".section-body{padding:2px 16px 16px}\n"
    ".remote-options{margin:8px -16px 0}\n"
    ".remote-choice{display:flex;align-items:center;gap:11px;min-height:48px;margin:0;padding:0 16px;border-bottom:1px solid #f0f1f3;font-size:15px;color:#333;cursor:pointer}\n"
    ".remote-choice input{width:19px;height:19px;accent-color:#0077ac;flex:0 0 auto}\n"
    ".remote-panel{padding:4px 0 2px}\n"
    ".remote-panel[hidden]{display:none}\n"
    ".remote-panel-title{font-size:13px;font-weight:600;color:#0077ac;margin:16px 0 2px}\n"
    ".accent-line{width:26px;height:1px;background:#40a0da;margin-bottom:8px}\n"
    ".status-line{display:flex;align-items:center;gap:8px;font-size:13px;color:#707070;margin-bottom:4px}\n"
    ".status-line i{width:8px;height:8px;border-radius:50%;display:block}\n"
    ".status-line .on{background:#34c759}\n"
    ".status-line .off{background:#ff3b30}\n"
    ".status-line .none{background:#c1c1c1}\n"
    "label{display:block;font-size:12px;color:#707070;margin:14px 0 5px}\n"
    "input[type=text],input[type=password],input[type=number],select{width:100%;padding:11px;font-size:15px;font-family:inherit;border:1px solid #e3e3e3;border-radius:6px;background:#f9f9f9}\n"
    "input:focus{outline:none;border-color:#0077ac;background:#fff}\n"
    "input::placeholder{color:#e67e22;opacity:1;font-style:italic}\n"
    ".broker-link{display:inline-block;margin-top:12px;color:#0077ac;font-size:13px;text-decoration:none}\n"
    ".hint{font-size:12px;color:#707070;margin-top:10px}\n"
    ".chk{display:flex;align-items:center;gap:8px;margin-top:14px;font-size:14px;color:#333;cursor:pointer}\n"
    ".chk input{width:18px;height:18px;accent-color:#0077ac}\n"
    ".btn{display:block;width:100%;padding:13px;border:none;border-radius:6px;font-size:14px;font-family:inherit;cursor:pointer;letter-spacing:.3px;margin-top:18px}\n"
    ".btn-save{background:#0077ac;color:#fff}\n"
    ".btn-save:active{background:#005f8c}\n"
    ".btn-back{background:#fff;color:#0077ac;box-shadow:0 1px 3px rgba(0,0,0,.06);text-decoration:none;text-align:center}\n"
    ".saved-banner{background:#e8f7ee;color:#1a7f37;text-align:center;font-size:13px;padding:10px;border-radius:8px;margin:8px 16px}\n"
    "</style></head><body><div class='app'>"
    "<div class='appbar'><div class='brand'>"
    "<svg viewBox='0 0 60 24' fill='none'><circle cx='30' cy='12' r='10.5' stroke='#333' stroke-width='1.5'/><path d='M14 12h9m14 0h9' stroke='#333' stroke-width='1.5'/><path d='M20 8.5l-3 3.5 3 3.5M40 8.5l3 3.5-3 3.5' stroke='#333' stroke-width='1.2' fill='none'/></svg>"
    "<span>Leaf Control</span></div></div>"
    "<div class='screens'>\n"
  ));

  if (saveResult != nullptr) {
    server.sendContent(F("<div class='saved-banner'>"));
    server.sendContent(saveResult);
    server.sendContent(F("</div>\n"));
  }

  server.sendContent(F("<form action='/settings_save' method='POST'><div class='settings-list'>\n"));
  server.sendContent(F("<details class='section' data-section='remote'><summary>Удалённое подключение</summary><div class='section-body'>\n"
    "<div class='remote-options'>"));
  RemoteTransport remoteMode = getRemoteTransport();
  server.sendContent(F("<label class='remote-choice'><input type='radio' name='remote_mode' value='0'"));
  if (remoteMode == RemoteTransport::OFF) server.sendContent(F(" checked"));
  server.sendContent(F("><span>Без подключения</span></label>\n"
    "<label class='remote-choice'><input type='radio' name='remote_mode' value='1'"));
  if (remoteMode == RemoteTransport::MQTT) server.sendContent(F(" checked"));
  server.sendContent(F("><span>MQTT-брокер</span></label>\n"
    "<label class='remote-choice'><input type='radio' name='remote_mode' value='2'"));
  if (remoteMode == RemoteTransport::OVMS_V2) server.sendContent(F(" checked"));
  server.sendContent(F("><span>OVMS Server V2</span></label></div>\n"
    "<div class='remote-panel' data-remote-panel='1' hidden>\n"
    "<div class='remote-panel-title'>MQTT-брокер</div>\n"));

  server.sendContent(F("<div class='status-line'>"));
  if (strlen(getMqttServer()) == 0) {
    server.sendContent(F("<i class='none'></i>Не настроен"));
  } else if (mqttConnected) {
    server.sendContent(F("<i class='on'></i>Подключено"));
  } else {
    server.sendContent(F("<i class='off'></i>Нет подключения"));
  }
  server.sendContent(F("</div><a class='broker-link' href='https://dash.wqtt.ru/broker/' target='_blank' rel='noopener'>Открыть панель брокера</a>\n"));

  server.sendContent(F(
    "<label for='mqtt_server'>Сервер (IP или имя хоста)</label>\n"
    "<input type='text' id='mqtt_server' name='mqtt_server' value='"
  ));
  sendNonEmpty(server, htmlEscapeAttr(getMqttServer()));
  server.sendContent(F(
    "' placeholder='m5.wqtt.ru' autocapitalize='off'>\n"
    "<label for='mqtt_port'>Порт</label>\n"
    "<input type='number' id='mqtt_port' name='mqtt_port' min='1' max='65535' value='"
  ));
  server.sendContent(String(getMqttPort()));
  server.sendContent(F(
    "' placeholder='15868'>\n"
    "<label for='mqtt_user'>Пользователь</label>\n"
    "<input type='text' id='mqtt_user' name='mqtt_user' value='"
  ));
  sendNonEmpty(server, htmlEscapeAttr(getMqttUser()));
  server.sendContent(F(
    "' placeholder='u_XXXXXX' autocapitalize='off'>\n"
    "<label for='mqtt_password'>Пароль (пусто — не менять)</label>\n"
    "<input type='password' id='mqtt_password' name='mqtt_password' value='' placeholder='••••••••'>\n"
    "<label class='chk'><input type='checkbox' name='mqtt_tls' value='1'"
  ));
  if (getMqttTls()) server.sendContent(F(" checked"));
  server.sendContent(F(
    "> Использовать TLS (порт 8883)</label>\n"
    "<label class='chk'><input type='checkbox' name='hass_discovery' value='1'"
  ));
  if (getHassDiscoveryEnabled()) server.sendContent(F(" checked"));
  server.sendContent(F(
    "> Автонастройка Home Assistant</label>\n"
    "</div>\n"
    "<div class='remote-panel' data-remote-panel='2' hidden>\n"
    "<div class='remote-panel-title'>OVMS Server V2</div>\n"
    "<label for='vehicle_id'>Vehicle ID</label><input type='text' id='vehicle_id' name='vehicle_id' value='"
  ));
  sendNonEmpty(server, htmlEscapeAttr(getVehicleId()));
  server.sendContent(F("' placeholder='MYLEAF' autocapitalize='off'>\n"
    "<label for='ovms_server'>Сервер</label><input type='text' id='ovms_server' name='ovms_server' value='"));
  sendNonEmpty(server, htmlEscapeAttr(getOvmsServer()));
  server.sendContent(F("' placeholder='api.openvehicles.com' autocapitalize='off'>\n"
    "<label for='ovms_port'>Порт</label><input type='number' id='ovms_port' name='ovms_port' min='1' max='65535' value='"));
  server.sendContent(String(getOvmsPort()));
  server.sendContent(F("'>\n"
    "<label for='ovms_password'>Server password (пусто — не менять)</label>"
    "<input type='password' id='ovms_password' name='ovms_password' value='' placeholder='••••••••'>\n"
    "<label for='ovms_module_password'>Пароль модуля (пусто — не менять)</label>"
    "<input type='password' id='ovms_module_password' name='ovms_module_password' value='' placeholder='••••••••'>\n"
    "<label class='chk'><input type='checkbox' name='ovms_tls' value='1'"));
  if (getOvmsTls()) server.sendContent(F(" checked"));
  server.sendContent(F("> Использовать TLS</label>\n"
    "<div class='status-line'>"));
  if (strlen(getOvmsServer()) == 0 || strlen(getOvmsPassword()) == 0) {
    server.sendContent(F("<i class='none'></i>Не настроен"));
  } else if (ovmsConnected) {
    server.sendContent(F("<i class='on'></i>Подключено"));
  } else {
    server.sendContent(F("<i class='off'></i>Нет подключения"));
  }
  server.sendContent(F("</div>\n"
    "<div class='hint'>Server password — пароль автомобиля на сервере. Пароль модуля должен совпадать с полем Module Password в приложении и включает paranoid-шифрование данных и команд.</div>\n"
    "<div class='hint'>TLS доступен через Wi-Fi. Текущий SIM800 TCP-клиент поддерживает OVMS V2 только без TLS.</div>\n"
    "</div>\n"
    "<button type='submit' class='btn btn-save'>Сохранить и перезагрузить</button>\n"
    "</div></details>\n"
    "<details class='section' data-section='wifi'><summary>Wi-Fi</summary><div class='section-body'>\n"
    "<label for='wifi_ssid'>Сеть 1 (SSID) — существующий профиль</label>\n"
    "<input type='text' id='wifi_ssid' name='wifi_ssid' value='"
  ));
  sendNonEmpty(server, htmlEscapeAttr(getWifiSSID()));
  server.sendContent(F(
    "' placeholder='Название вашей Wi-Fi сети' autocapitalize='off'>\n"
    "<label for='wifi_password'>Пароль сети 1 (пусто — не менять)</label>\n"
    "<input type='password' id='wifi_password' name='wifi_password' value='' placeholder='••••••••'>\n"
    "<label for='wifi_ssid2'>Сеть 2 (SSID)</label>\n"
    "<input type='text' id='wifi_ssid2' name='wifi_ssid2' value='"
  ));
  sendNonEmpty(server, htmlEscapeAttr(getWifiSSID2()));
  server.sendContent(F(
    "' placeholder='Вторая сеть' autocapitalize='off'>\n"
    "<label for='wifi_password2'>Пароль сети 2 (пусто — не менять)</label>\n"
    "<input type='password' id='wifi_password2' name='wifi_password2' value='' placeholder='••••••••'>\n"
    "<label for='wifi_ssid3'>Сеть 3 (SSID)</label>\n"
    "<input type='text' id='wifi_ssid3' name='wifi_ssid3' value='"
  ));
  sendNonEmpty(server, htmlEscapeAttr(getWifiSSID3()));
  server.sendContent(F(
    "' placeholder='Третья сеть' autocapitalize='off'>\n"
    "<label for='wifi_password3'>Пароль сети 3 (пусто — не менять)</label>\n"
    "<input type='password' id='wifi_password3' name='wifi_password3' value='' placeholder='••••••••'>\n"
    "<label for='ap_ssid'>Точка доступа (SSID) — пусто = точка доступа выключена</label>\n"
    "<input type='text' id='ap_ssid' name='ap_ssid' value='"
  ));
  sendNonEmpty(server, htmlEscapeAttr(getApSSID()));
  server.sendContent(F(
    "' placeholder='LeafControl-AP' autocapitalize='off'>\n"
    "<label for='ap_password'>Пароль точки доступа (мин. 8 симв., пусто — не менять)</label>\n"
    "<input type='password' id='ap_password' name='ap_password' value='' placeholder='••••••••'>\n"
    "<div class='hint'>Плата выбирает доступную сохранённую сеть с лучшим сигналом и переключается на другой профиль при потере связи. Пароли не отображаются и не стираются при пустом поле.</div>\n"
    "<button type='submit' class='btn btn-save'>Сохранить и перезагрузить</button>\n"
    "</div></details>\n"
    "<details class='section' data-section='security'><summary>Безопасность</summary><div class='section-body'>\n"
    "<label class='chk' style='margin-top:0'><input type='checkbox' name='locking_enabled' value='1'"
  ));
  if (getLockingEnabled()) server.sendContent(F(" checked"));
  server.sendContent(F(
    "> Разрешить удалённую блокировку/разблокировку</label>\n"
    "<div class='hint'>Команды lock/unlock работают только при включённом пункте. Не включайте, если в салоне могут оставаться дети или животные: плата сможет закрыть двери дистанционно.</div>\n"
    "<button type='submit' class='btn btn-save'>Сохранить и перезагрузить</button>\n"
    "</div></details>\n"
    "<details class='section' data-section='modem'><summary>Мобильный модем</summary><div class='section-body'>\n"
    "<label class='chk' style='margin-top:0'><input type='checkbox' id='modem_sim800' name='modem_sim800' value='1'"
  ));
  if (getModemEnable() && getModemType() == 0) server.sendContent(F(" checked"));
  server.sendContent(F(
     ">SIM800 (2G, GPRS)</label>\n"
    "<div id='m800box'>\n"
    "<label for='modem800_apn'>APN (SIM800)</label>\n"
    "<input type='text' id='modem800_apn' name='modem800_apn' value='"
  ));
  sendNonEmpty(server, htmlEscapeAttr(getModem800Apn()));
  server.sendContent(F(
    "' placeholder='internet' autocapitalize='off'>\n"
     "<label for='modem800_user'>APN пользователь (пусто — не менять)</label>\n"
     "<input type='text' id='modem800_user' name='modem800_user' value='"
   ));
   sendNonEmpty(server, htmlEscapeAttr(getModem800User()));
   server.sendContent(F(
     "' autocapitalize='off'>\n"
     "<label for='modem800_pass'>APN пароль (пусто — не менять)</label>\n"
     "<input type='password' id='modem800_pass' name='modem800_pass' value='' placeholder='"
   ));
    server.sendContent(strlen(getModem800Pass()) > 0 ? "сохранён — пусто не менять" : "не задан");
    server.sendContent(F("'>\n"
     "<div class='call-settings'>\n"
     "<label class='chk'><input type='checkbox' name='modem_call_enable' value='1'"
   ));
   if (getModemCallEnabled()) server.sendContent(F(" checked"));
   server.sendContent(F(
     "> Управление климатом входящим звонком</label>\n"
     "<div class='hint'>Подключение SIM800: TXD → GPIO35, RXD → GPIO38, RING/RI → GPIO37. Первый разрешённый звонок включает климат, следующий выключает; остальные вызовы отклоняются. Русские голосовые фразы загружаются во внутреннюю память модема автоматически.</div>\n"
   ));
   for (uint8_t i = 0; i < 5; i++) {
     server.sendContent(F("<label for='modem_call_num"));
     server.sendContent(String(i + 1));
     server.sendContent(F("'>Разрешённый номер "));
     server.sendContent(String(i + 1));
     server.sendContent(F("</label><input type='text' inputmode='tel' autocomplete='tel' id='modem_call_num"));
     server.sendContent(String(i + 1));
     server.sendContent(F("' name='modem_call_num"));
     server.sendContent(String(i + 1));
     server.sendContent(F("' value='"));
     sendNonEmpty(server, htmlEscapeAttr(getModemCallNumber(i)));
     server.sendContent(F("' placeholder='+79991234567'>\n"));
   }
   server.sendContent(F(
     "</div>\n"
     "</div>\n"
    "<label for='modem_pwrkey'>GPIO PWRKEY (0 — питание не управляется)</label>\n"
    "<input type='number' id='modem_pwrkey' name='modem_pwrkey' min='0' max='48' value='"
  ));
  server.sendContent(String(getModemPwrKey()));
  server.sendContent(F(
    "'>\n"
    "<div class='hint'>UART SIM800: TXD модема → GPIO35, RXD модема → GPIO38. RING/RI → GPIO37. Wi-Fi имеет приоритет для MQTT. При включённом управлении звонком SIM800 остаётся зарегистрированным в GSM. SIM800 требует сети 2G.</div>\n"
    "<button type='submit' class='btn btn-save'>Сохранить и перезагрузить</button>\n"
    "</div></details>\n"
    "<details class='section' data-section='power'><summary>Экономия энергии</summary><div class='section-body'>\n"
    "<label class='chk' style='margin-top:0'><input type='checkbox' name='sleep_enable' value='1'"
  ));
  if (getSleepEnabled()) server.sendContent(F(" checked"));
  server.sendContent(F(
    "> Глубокий сон при простое машины</label>\n"
    "<label for='sleep_after_h'>Уходить в сон после (часов простоя)</label>\n"
    "<input type='number' id='sleep_after_h' name='sleep_after_h' min='1' max='168' value='"
  ));
  server.sendContent(String(getSleepAfterHours()));
  server.sendContent(F(
    "'>\n"
    "<label for='wake_fast_min'>Пробуждаться каждые (мин, до 48 ч простоя)</label>\n"
    "<input type='number' id='wake_fast_min' name='wake_fast_min' min='1' max='1440' value='"
  ));
  server.sendContent(String(getWakeFastMinutes()));
  server.sendContent(F(
    "'>\n"
    "<label for='wake_slow_min'>Пробуждаться каждые (мин, после 48 ч простоя)</label>\n"
    "<input type='number' id='wake_slow_min' name='wake_slow_min' min='1' max='1440' value='"
  ));
  server.sendContent(String(getWakeSlowMinutes()));
  server.sendContent(F(
    "'>\n"
    "<button type='submit' class='btn btn-save'>Сохранить и перезагрузить</button>\n"
    "</div></details></div></form>\n"
    "<div style='padding:0 16px'><a href='/' class='btn btn-back'>Назад</a></div>\n"
    "<script>function showRemote(){var r=document.querySelector('input[name=remote_mode]:checked'),v=r?r.value:'0';document.querySelectorAll('[data-remote-panel]').forEach(function(p){p.hidden=p.dataset.remotePanel!==v})}document.querySelectorAll('input[name=remote_mode]').forEach(function(r){r.addEventListener('change',showRemote)});showRemote();document.querySelectorAll('.section').forEach(function(s){s.addEventListener('toggle',function(){if(!s.open)return;document.querySelectorAll('.section[open]').forEach(function(o){if(o!==s)o.open=false});history.replaceState(null,'','#'+s.dataset.section)})});var h=location.hash.slice(1),d=h&&document.querySelector('[data-section=\"'+h+'\"]');if(d)d.open=true;</script>\n"
    "</div></div></body></html>\n"
  ));

  server.sendContent("");
}

#endif
