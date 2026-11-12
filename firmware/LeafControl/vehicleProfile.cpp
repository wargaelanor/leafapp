#include "vehicleProfile.h"
#include "configuration.h"

static unsigned long lastZe1Ms = 0;
static unsigned long lastAze0Ms = 0;
static const unsigned long PROFILE_TTL_MS = 180000UL;   // 3 минуты

void leafNoteZe1Frame() {
  unsigned long now = millis();
  lastZe1Ms = now ? now : 1;
}

void leafNoteAze0Frame() {
  unsigned long now = millis();
  lastAze0Ms = now ? now : 1;
}

bool leafIsZe1() {
  int mode = getVehicleProfile();
  if (mode == 1) return true;
  if (mode == 2) return false;

  unsigned long now = millis();
  if (lastZe1Ms && now - lastZe1Ms < PROFILE_TTL_MS) return true;
  if (lastAze0Ms && now - lastAze0Ms < PROFILE_TTL_MS) return false;
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
