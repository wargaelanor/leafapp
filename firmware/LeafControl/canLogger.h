#ifndef CAN_LOGGER_H
#define CAN_LOGGER_H

#include <Arduino.h>

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

#endif
