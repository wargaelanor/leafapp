#ifndef SERVER_API_H
#define SERVER_API_H

#include <Arduino.h>

// Связь платы с собственным сервером LeafControl (замена MQTT/OVMS).
//
// Первый запуск: плата генерирует свой логин и пароль, печатает их в serial
// и регистрируется на сервере (POST /api/device/enroll). На сервере устройство
// появляется в статусе pending, админ одобряет его в веб-интерфейсе.
//
// Дальше плата раз в несколько секунд отправляет состояние, раз в 3 секунды
// опрашивает очередь команд и подтверждает выполнение.

void serverApiBegin();
void serverApiTick();

// true, если включена связь с сервером и есть Wi-Fi
bool serverApiReady();

// Логин/пароль, которые плата выдала при первом запуске (для показа в UI).
const char* serverApiLogin();
const char* serverApiPassword();
const char* serverApiStatusText();
const char* serverApiBase();
const char* serverApiClaimCode();

// Обработчик команд с сервера. Реализуется в скетче.
typedef void (*ServerCommandHandler)(const char* name, const char* argsJson);
void serverApiSetCommandHandler(ServerCommandHandler handler);

// Запись завершённой поездки/зарядки ("trip"/"charge", JSON-строка платы).
// Вызывается из tripLog; ставит запись в очередь отправки.
void serverApiOnRecord(const char* kind, const char* line);

#endif
