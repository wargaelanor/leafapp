#ifndef MODEM_PROBE_H
#define MODEM_PROBE_H

#include <Arduino.h>

// Диагностика модема SIM800 по шагам: UART -> SIM -> регистрация в сети ->
// GPRS (APN, PDP-контекст, IP) -> реальный TCP до сервера.
//
// Проверка идёт в отдельной задаче, потому что GPRS-атенн может занимать до
// минуты. Веб-сервер в это время свободен: POST запускает проверку, GET
// забирает результат.

// Запускает проверку. Повторный вызов во время работы ничего не делает.
void modemProbeStart();

// true, пока проверка выполняется.
bool modemProbeActive();

// Последний результат в виде JSON-строки (валиден и во время работы).
String modemProbeJson();

// Пульс PWRKEY (активный низкий уровень), когда пин задан.
void modemProbePowerPulse();

// 0, если пин PWRKEY не настроен.
int modemProbePwrKeyPin();

#endif
