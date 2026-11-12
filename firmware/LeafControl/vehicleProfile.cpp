#include "vehicleProfile.h"
#include "configuration.h"

static bool sawZe1 = false;          // 0x59E когда-либо приходил (только ZE1)
static unsigned long lastAze0Ms = 0;
static const unsigned long AZE0_TTL_MS = 120000UL;   // 2 минуты

void leafNoteZe1Frame() {
  sawZe1 = true;
}

void leafNoteAze0Frame() {
  unsigned long now = millis();
  lastAze0Ms = now ? now : 1;
}

bool leafIsZe1() {
  int mode = getVehicleProfile();
  if (mode == 1) return true;
  if (mode == 2) return false;

  // 0x59E (SOC приборки) — уникальный признак ZE1: увидели хоть раз, значит ZE1.
  if (sawZe1) return true;
  unsigned long now = millis();
  if (lastAze0Ms && now - lastAze0Ms < AZE0_TTL_MS) return false;
  // Пока шина молчит — считаем машину ZE1: это текущая проверенная плата.
  return true;
}

const char* leafProfileName() {
  return leafIsZe1() ? "ze1" : "aze0";
}

const char* leafProfileModeName() {
  switch (getVehicleProfile()) {
    case 1: return "ze1";
    case 2: return "aze0";
    default: return "auto";
  }
}
