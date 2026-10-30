# LeafControl — Nissan Leaf ZE1 на LilyGO T-2CAN (ESP32-S3)

Прошивка телеметрии и управления для Nissan Leaf ZE1: читает обе CAN-шины,
считает ток/мощность/заряд, умеет включать климат и зарядку, отдаёт данные в
приложение и на сервер.

## Что умеет

- **EV-CAN (Port A, listen-only)**: напряжение, ток, мощность, SOC,
  одометр, температура, режим машины. Автоопределение тайминга
  (16/8/20 МГц × 500/250/125k) с возвратом на 16 МГц / 500k и повтором
  каждые 30 с, пока шина не поймана.
- **CAR-CAN (Port B)**: команды `0x56E`/`0x68C` — климат, зарядка,
  lock/unlock, lights; диагностика BMS через шлюз (ISO-TP, группы
  `0x04`/`0x01`/`0x61` — температура, батарея, SOH).
- **Связь**: три режима на выбор — без подключения, MQTT-брокер,
  OVMS Server V2 (MP-C/MP-S, HMAC-MD5 + RC4-drop-1024, paranoid). У MQTT
  и OVMS **свой Vehicle ID** — топики и логины не пересекаются.
- **Веб-интерфейс** (PWA, встроен в прошивку и лежит в корне репозитория):
  одна иконка обновления в шапке, честный `—` для устаревших данных
  (`fage >= 30`), настройки Wi-Fi (3 профиля), брокера и сервера.
- **Прочее**: deep sleep, журнал CAN на FAT-раздел, web-OTA, developer-API
  (пароль `1985`, HttpOnly-кука `leafdev`).

## Сборка

```powershell
# PWA -> встроенная копия (firmware/LeafControl/webapp_html.h)
powershell -File tools\mkwebapp.ps1

arduino-cli compile ^
  --config-file "$env:USERPROFILE\.arduinoIDE\arduino-cli.yaml" ^
  --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=disabled" ^
  --output-dir "%TEMP%\leafbin-ze1" firmware\LeafControl
```

## Прошивка по сети

```powershell
powershell -File tools\ota-retry.ps1 -Firmware "$env:TEMP\leafbin-ze1\LeafControl.ino.bin" `
  -ExpectedVersion "FW-YYYY.MM.DD.NN-CMD"
```

Скрипт опрашивает статус только до и после передачи: любой параллельный
запрос к плате во время заливки обрывает её. Плата ищется по `leaf.local`,
потому что адрес зависит от того, к какой сети она подключилась.

## Проверка на машине

```powershell
powershell -File tools\verify-bms.ps1 -Minutes 6
```

Логинится в developer-API, запускает read-only пробу VIN, включает климат и
печатает `q/r/grp/req/age/cd/fail/btemp/v/i` каждые 10 секунд.

## Ограничения, о которых нужно знать

- **VIN**: блок батареи (`0x797`, `02 21 81`) на этой машине возвращает
  17 пробелов — VIN в LBC нет. Запасной путь — штатный OBD-II `09 02` на
  `0x7DF` (ответ `0x7E8`), он вынесен в отдельную read-only пробу
  `POST /api/developer/vin_probe` и по умолчанию заблокирован в whitelist.
- **Температура BMS и SOH** приходят только пока шлюз на CAR-CAN бодрствует
  (движение, зарядка, remote climate). У стоящей машины шина молчит, и
  значения честно показываются как `—`.
- **Остановка зарядки** на ZE1 делается MITM-кадром `0x1DB` на EV-CAN;
  при listen-only Port A недоступна — это архитектурное ограничение.
- Официальное приложение OVMS видит команды только в режиме
  **OVMS Server V2**; в режиме MQTT-брокера топики другие.

## Демонстрация для клиентов

`demo\leafcontrol-demo.html` — одна самодостаточная страница (картинки внутри,
данные условные): машина сама переключает состояния «покой → свет → зарядка →
движение» каждые 3 секунды, есть кнопки и переключатель темы. Файл можно
отправить одним вложением, открыть офлайн или положить на любой хостинг.

Пересобрать после замены картинок:

```powershell
powershell -File tools\mkdemo.ps1
```
## Документация

- `docs/SESSION_REPORT.md` — что проверено на машине, с цифрами и выводами.
- `docs/OVMS_REFERENCE.md` — кадры и payload'ы ZE1 по OVMS.
- `docs/RELEASES.md` — история версий прошивки и PWA.

## Лицензия

Код прошивки и веб-интерфейса — для личного использования владельцем
автомобиля. Справочные данные по кадрам взяты из открытого проекта OVMS.
