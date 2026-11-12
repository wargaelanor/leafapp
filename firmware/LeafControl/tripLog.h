#ifndef TRIP_LOG_H
#define TRIP_LOG_H

#include <Arduino.h>

// Trip and charge history kept on the board itself (FAT partition), so it does
// not depend on the phone being open. One JSON line per finished record.
void tripLogInit();
void tripLogTick();

// Закрыть открытую поездку (перед сном/перезагрузкой).
void tripLogFinishOpen(const char* reason);

bool tripLogIsDriving();     // true while a trip is being recorded
bool tripLogIsCharging();    // true while a charge session is being recorded

// Идёт активная сессия (поездка/климат/зарядка/машина включена) — по ней
// CanLogger пишет CAN-лог для разбора.
bool tripLogSessionActive();

// Отметка: запись с таким временем окончания уже ушла на сервер.
void tripLogNoteRecordSent(uint32_t endEpoch);

void tripLogSendTrips();     // stream the newest records as a JSON array
void tripLogSendCharges();
void tripLogClearTrips();
void tripLogClearCharges();

// Day/night tariff window used when the board splits energy. Default 00:00-07:00.
void tripLogSetNightWindow(uint8_t fromHour, uint8_t toHour);
uint8_t tripLogNightFrom();
uint8_t tripLogNightTo();

// Full tariff (prices are only displayed by clients; the board stores them so
// the settings page and the app always agree).
void tripLogSetTariff(double dayPrice, double nightPrice, uint8_t fromHour, uint8_t toHour);
double tripLogTariffDay();
double tripLogTariffNight();

size_t tripLogTripsBytes();
size_t tripLogChargesBytes();
size_t tripLogTripsCount();
size_t tripLogChargesCount();

#endif
