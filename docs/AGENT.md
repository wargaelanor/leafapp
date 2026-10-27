# Заметки для ИИ-агента (и разработчика)

Репозиторий: Leaf Control — PWA (корень, GitHub Pages) + прошивка
ESP32-S3 (`firmware/LeafControl/`) + инструменты (`tools/`) +
документация (`docs/`).

## Структура

- `index.html` — весь фронтенд PWA (один файл, JS инлайном).
- `sw.js` — service worker (v4+; network-first навигация).
- `firmware/LeafControl/LeafControl.ino` — точка входа прошивки,
  web-роуты, `handleSettingsSave`, OTA, sleep, loop.
- `firmware/LeafControl/ui.h` — серверная генерация страницы настроек.
- `firmware/LeafControl/configuration.cpp` — NVS-конфигурация
  (классы `ConfigString/ConfigInt/ConfigBool`, `initConfiguration`).
- `firmware/LeafControl/mqttInterface.cpp` — MQTT, топики команд/ответов.
- `firmware/LeafControl/modemManager.cpp` — GSM-модем (attach при
  пропадании Wi-Fi), `modemHwSim800.cpp`/`modemHwSim7600.cpp`.
- `firmware/LeafControl/webapp_html.h` — ГЕНЕРИРУЕМЫЙ файл (веб-UI
  платы внутри прошивки), не редактировать руками.

## Сборка и прошивка

```powershell
# 0) сгенерировать webapp_html.h, если менялся index.html
powershell -File tools\mkwebapp.ps1

# 1) компиляция (arduino-cli должен быть установлен)
& "$env:LOCALAPPDATA\Temp\opencode\arduino-cli\arduino-cli.exe" compile `
  --config-file "$env:USERPROFILE\.arduinoIDE\arduino-cli.yaml" `
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc" `
  --output-dir "$env:TEMP\leafbin" firmware\LeafControl

# 2a) web-OTA (плата в той же сети; URL берите из последнего лога
#     или mDNS http://leaf.local — IP по DHCP меняется!)
curl.exe -m 240 -F "firmware=@$env:TEMP\leafbin\LeafControl.ino.bin" http://leaf.local/ota_update

# 2b) USB-прошивка: arduino-cli upload -p COMxx --fqbn "..." firmware\LeafControl

# 3) serial-лог (115200) — на порт не должен держать другой процесс
```

Размер: 1 289 869 / 1 310 720 байт flash (**98%**, свободно ~21 КБ). Новый
код почти не влезает — после правок ВСЕГДА смотрите строку «Скетч
использует …», сокращайте если выросло. Основные «якоря» размера: PNG
машины 23.5 КБ (`leaf.png` в PROGMEM) и HTML PWA ~71 КБ. Если нужно место —
`leaf1.png` (та же картинка 240×134, 12 КБ) или пересжатие PNG в 8 бит.

## Serial-консоль (CLI)

Порт 115200, **DTR/RTS = false** при открытии — иначе плата уходит в
ROM-загрузчик и печатает только `ESP-ROM:esp32s3-...`. Включение DTR
после открытия тоже ломает связь. Команды завершаются `\n`.

| Команда | Что делает |
| --- | --- |
| `status` | Всё декодированное состояние авто + `CAN frames: Port B / Port A` |
| `log on` / `log off` | Пофреймовый лог CAN (`<`/`>`/`!` + ID) |
| `list` / `list --secrets` | Текущая конфигурация из NVS |
| `get <key>` / `set <key> <val>` | Чтение/запись отдельного параметра |
| `reboot` | Перезагрузка |
| `factory-reset` | Сброс NVS |

Диагностика шин: на выключенной машине `canFramesB`/`canFramesA` стоят на
нуле — это нормально (нет активности на IT-CAN/EV-CAN). В аксессуаре или
READY оба счётчика должны расти; если Port A растёт, а Port B нет —
смотреть питание/подключение MCP2515 и TWAI-пины.

## Проверки после изменений

1. Компиляция без ошибок и без заметного роста размера.
2. `GET http://leaf.local/api/status` → JSON.
3. `GET http://leaf.local/settings` → ожидаемый HTML.
4. OTA → плата отвечает, в serial `### OTA success: N bytes ###`.
5. Для PWA: правки проверяются локально, деплой = `git push` в `main`
   (GitHub Pages).

## Ловушки и конвенции

- **`ui.h`: условный атрибут (`checked` и т.п.) вставляется только
  ПОСЛЕ основной строки `<input ...` и ДО её закрывающего `>`:**
  ```
  "<input ... onchange=\"...\""      // без '>'
  ));  if (...) sendContent(F(" checked"));
  sendContent(F(">Подпись</label>"));
  ```
  Закрытие тега раньше `checked` превращает атрибут в текст —
  баг от 22.10.2026 (галка модема не отображалась).
- **`/settings_save` (POST всей формы)**: отсутствующий чекбокс =
  выключен (`server.hasArg`); пустое поле пароля = не менять;
  пустой SSID = выключить; пароль AP минимум 8 символов (иначе
  отклоняется с логом); POST без аргументов СБРАСЫВАЕТ настройки —
  всегда отправляйте полную форму.
- **Модем**: поднимается только когда Wi-Fi падает >60 с; требует
  заполненного APN; `PWRKEY=0` — питание не управляется (модуль должен
  включаться сам от VCC); SIM800 требует 2G. Состояние — в
  serial-логе `[modem] …`.
- **Sleep**: счётчик простоя в RTC растёт и НЕ сбрасывается от
  открытия страницы; после ухода в сон — окно ~90 с каждые
  N минут (по умолчанию 10, после 48 ч — 60). Плата может временно
  не отвечать по сети — это не обязательно поломка. Полное
  энергопотребление и таймеры — в разделе «Экономия энергии» веб-UI.
- **OTA-обрывы**: upload периодически обрывается (фиксированный
  обрыв около 640 КБ) — повторите; если повтор падает, откройте
  serial-лог (115200) и смотрите `### OTA …`.
- **IP платы по DHCP меняется** — используйте `http://leaf.local`
  (mDNS) или смотрите IP в boot-логе.
- **Пароли**: в UI отображаются маской, в NVS хранятся открытым
  текстом; в репозиторий секреты не коммитить.
- **Команды платы — только POST** (защита от CSRF): см. роуты
  `server.on("/lock", HTTP_POST, …)` в `LeafControl.ino`; в PWA —
  `fetch(..., {method:'POST'})`. Топики MQTT — в `mqttInterface.cpp`.
- **Формат коммитов PWA**: английское однострочное описание +
  маркер версии в скобках, например `… (leaf-v11)`.

## Сеть/брокер

Клиенты общаются с платой напрямую (REST, локальный режим) или через
MQTT-брокер (TLS, логин/пароль из настроек платы, тема —
`vehicle_id`). Home Assistant discovery включается настройкой
«HASS discovery».
