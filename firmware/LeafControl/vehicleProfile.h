#ifndef VEHICLE_PROFILE_H
#define VEHICLE_PROFILE_H

#include <Arduino.h>

// Профиль машины. По умолчанию — автоопределение по шине:
//   * кадр 0x59E (SOC приборки) приходит только на ZE1 (2018+);
//   * на AZE0/ZE0 (2011-2017) SOC лежит в 0x1DB, байт 4.
// Ручной выбор (0=авто, 1=ZE1, 2=AZE0) хранится в NVS и перекрывает авто.
bool leafIsZe1();
const char* leafProfileName();       // "ze1" | "aze0"
const char* leafProfileModeName();   // "auto" | "ze1" | "aze0"

// Отметки от декодеров шины для автоопределения.
void leafNoteZe1Frame();
void leafNoteAze0Frame();

#endif
