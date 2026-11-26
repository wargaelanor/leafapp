#ifndef SIM_TRAFFIC_H
#define SIM_TRAFFIC_H

#include <stdint.h>

// Учёт трафика SIM по HTTP-запросам модема. Счётчики хранятся в отдельном
// пространстве NVS, поэтому не сбрасываются factory reset настроек.
struct SimTrafficInfo {
  float dayMb;
  float weekMb;
  float monthMb;
  float totalMb;
  float limitMb;
  bool  blocked;
};

void simTrafficInit();
void simTrafficTick();
void simTrafficAdd(uint32_t bytes);
void simTrafficRefresh();
SimTrafficInfo simTrafficInfo();
uint32_t simTrafficTotalKb();
bool simTrafficBlocked();
void simTrafficResetCounters();

#endif
