#include "ovmsV2Client.h"

// OVMS Server V2 удалён: остаётся только собственный сервер (serverApi).
// Заглушки, чтобы не ломать ссылки из скетча и веб-интерфейса.

void setupOvmsV2() {}
void manageOvmsV2() {}
void ovmsV2Disconnect() {}
bool ovmsV2IsConnected() { return false; }
bool ovmsV2ParanoidIsActive() { return false; }
