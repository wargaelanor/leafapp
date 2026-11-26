#include "simTraffic.h"
#include "configuration.h"

#include <Preferences.h>
#include <time.h>

// Счётчики в байтах: дню/неделе/месяцу соответствуют "окна" по локальному
// времени платы. Месяц используется для лимита, остальные окна — для
// статистики за 24 ч / 7 дней на самой плате.
namespace {

Preferences prefs;
bool prefsReady = false;

uint32_t dayBytes = 0;
uint32_t weekBytes = 0;
uint32_t monthBytes = 0;
uint32_t totalBytes = 0;

int dayKey = 0;
int weekKey = 0;
int monthKey = 0;
bool haveKeys = false;
bool blocked = false;
bool dirty = false;
uint32_t lastSaveMs = 0;
uint32_t lastLimitLogMs = 0;

const uint32_t SAVE_INTERVAL_MS = 60000UL;

bool localParts(int& day, int& week, int& month) {
  time_t now = time(nullptr);
  if (now < 1600000000L) return false;
  time_t local = now + (time_t)getTzOffset();
  struct tm tmv;
  if (!gmtime_r(&local, &tmv)) return false;
  day = (int)(local / 86400L);
  week = (int)(local / 604800L);
  month = tmv.tm_year * 12 + tmv.tm_mon;
  return true;
}

void save() {
  if (!prefsReady) return;
  prefs.putUInt("day_b", dayBytes);
  prefs.putUInt("week_b", weekBytes);
  prefs.putUInt("month_b", monthBytes);
  prefs.putUInt("total_b", totalBytes);
  prefs.putInt("day_k", dayKey);
  prefs.putInt("week_k", weekKey);
  prefs.putInt("month_k", monthKey);
  dirty = false;
  lastSaveMs = millis();
}

void maybeSave(bool force) {
  if (!dirty) return;
  if (force || (uint32_t)(millis() - lastSaveMs) >= SAVE_INTERVAL_MS) save();
}

}  // namespace

void simTrafficInit() {
  prefsReady = prefs.begin("simtraf", false);
  if (!prefsReady) {
    Serial.println("[simtraf] NVS unavailable, counters live in RAM only");
  } else {
    dayBytes = prefs.getUInt("day_b", 0);
    weekBytes = prefs.getUInt("week_b", 0);
    monthBytes = prefs.getUInt("month_b", 0);
    totalBytes = prefs.getUInt("total_b", 0);
    dayKey = prefs.getInt("day_k", 0);
    weekKey = prefs.getInt("week_k", 0);
    monthKey = prefs.getInt("month_k", 0);
    haveKeys = (prefs.getBytesLength("day_k") > 0);
  }
  simTrafficRefresh();
}

void simTrafficRefresh() {
  int day = 0, week = 0, month = 0;
  bool changed = false;
  if (localParts(day, week, month)) {
    if (!haveKeys) {
      dayKey = day;
      weekKey = week;
      monthKey = month;
      haveKeys = true;
    } else {
      if (day != dayKey) { dayKey = day; dayBytes = 0; changed = true; }
      if (week != weekKey) { weekKey = week; weekBytes = 0; changed = true; }
      if (month != monthKey) {
        monthKey = month;
        monthBytes = 0;
        changed = true;
        Serial.println("[simtraf] new billing month, counter reset");
      }
    }
  }

  const float limitMb = getSimTrafficLimitMb();
  const uint32_t limitBytes = limitMb > 0 ? (uint32_t)(limitMb * 1048576.0f) : 0UL;
  bool nowBlocked = limitBytes > 0 && monthBytes >= limitBytes;
  if (nowBlocked != blocked) {
    blocked = nowBlocked;
    if (blocked && (uint32_t)(millis() - lastLimitLogMs) > 5000UL) {
      Serial.printf("[simtraf] limit reached: %.1f MB / %.1f MB, SIM HTTP disabled\n",
                    monthBytes / 1048576.0f, limitMb);
      lastLimitLogMs = millis();
    }
    changed = true;
  }
  if (changed) { dirty = true; maybeSave(true); }
}

void simTrafficAdd(uint32_t bytes) {
  if (bytes == 0) return;
  simTrafficRefresh();
  dayBytes += bytes;
  weekBytes += bytes;
  monthBytes += bytes;
  totalBytes += bytes;
  dirty = true;
  simTrafficRefresh();
  maybeSave(false);
}

void simTrafficTick() {
  simTrafficRefresh();
  maybeSave(false);
}

SimTrafficInfo simTrafficInfo() {
  simTrafficRefresh();
  SimTrafficInfo out;
  out.dayMb = dayBytes / 1048576.0f;
  out.weekMb = weekBytes / 1048576.0f;
  out.monthMb = monthBytes / 1048576.0f;
  out.totalMb = totalBytes / 1048576.0f;
  out.limitMb = getSimTrafficLimitMb();
  out.blocked = blocked;
  return out;
}

uint32_t simTrafficTotalKb() {
  return totalBytes / 1024UL;
}

bool simTrafficBlocked() {
  simTrafficRefresh();
  return blocked;
}

void simTrafficResetCounters() {
  dayBytes = 0;
  weekBytes = 0;
  monthBytes = 0;
  totalBytes = 0;
  blocked = false;
  if (localParts(dayKey, weekKey, monthKey)) haveKeys = true;
  dirty = true;
  maybeSave(true);
}
