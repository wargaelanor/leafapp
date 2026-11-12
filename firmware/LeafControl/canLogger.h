#ifndef CAN_LOGGER_H
#define CAN_LOGGER_H

#include <Arduino.h>

// Ручная запись с веб-страницы платы (файл /canlog.bin).
bool canLoggerStart();
void canLoggerStop();
void canLoggerClear();
void canLoggerAppend(char bus, char direction, uint32_t id, const uint8_t* data, uint8_t len);
bool canLoggerIsRecording();
bool canLoggerIsFull();
bool canLoggerHasLog();
size_t canLoggerCount();
size_t canLoggerCapacity();
void canLoggerSendDownload();

// Автоматические CAN-сессии (тестовый режим): запись начинается, когда машина
// едет/включена или работает климат/зарядка, и заканчивается через 5 минут
// после выключения. Файлы /can_<epoch>.bin уходят на сервер и удаляются.
void canLoggerAutoTick(bool active);
bool canLoggerIsAuto();
size_t canLoggerSessionBytes();
size_t canLoggerPendingCount();

// Очередь выгрузки: самая старая неотправленная сессия.
bool canLoggerNextPending(char* out, size_t outSize, size_t* bytes);
bool canLoggerExists(const char* name);
size_t canLoggerReadAt(const char* name, size_t off, uint8_t* buf, size_t maxLen);
void canLoggerMarkSent(const char* name);

#endif
