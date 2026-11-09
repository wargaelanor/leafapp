// ===== Tier-1 UDS poller (Leaf2018-CAN.pdf) ==================================
// Rotates single-frame ReadDataByIdentifier requests over CAR-CAN while the
// car is awake. Never sends anything outside the diagnostic activity window,
// never collides with the BMS/VIN ISO-TP transfers, backs off any ECU that
// answers NRC or stays silent.

#include <Arduino.h>
#include "udsLeaf.h"
#include "canInterface.h"
#include "canLeafZE1.h"
#include "configuration.h"

UdsTier1Data udsData;

struct UdsEntry {
    const char* name;
    uint32_t    reqId;     // request address
    uint16_t    did;       // requested DID
    uint16_t    didResp;   // DID echoed in the answer (ECO answers 131B)
    uint16_t    periodSec;
    bool        tpms;      // only polled when the TPMS flag is on
};

// Response address = reqId + 3 for every ECU below.
static const UdsEntry kQueries[] = {
    {"gear",       0x797, 0x1156, 0x1156, 10,  false},
    {"v12",        0x797, 0x1103, 0x1103, 15,  false},
    {"plug",       0x797, 0x1234, 0x1234, 15,  false},
    {"speed",      0x797, 0x121A, 0x121A, 10,  false},
    {"i12",        0x797, 0x1183, 0x1183, 30,  false},
    {"chargemode", 0x797, 0x114E, 0x114E, 20,  false},
    {"obcpower",   0x797, 0x1236, 0x1236, 20,  false},
    {"ac",         0x797, 0x1106, 0x1106, 60,  false},
    {"eco",        0x797, 0x1318, 0x131B, 60,  false},
    {"epedal",     0x797, 0x131A, 0x131A, 60,  false},
    {"rearheater", 0x797, 0x110F, 0x110F, 120, false},
    {"motortemp",  0x784, 0x1121, 0x1121, 60,  false},
    {"brake",      0x740, 0x1209, 0x1209, 10,  false},
    {"accel",      0x740, 0x1115, 0x1115, 10,  false},
    {"swa",        0x740, 0x1208, 0x1208, 20,  false},
    {"qccount",    0x797, 0x1203, 0x1203, 3600, false},
    {"l1l2count",  0x797, 0x1205, 0x1205, 3600, false},
    {"tpms_fr",    0x743, 0x0E25, 0x0E25, 300, true},
    {"tpms_fl",    0x743, 0x0E26, 0x0E26, 300, true},
    {"tpms_rr",    0x743, 0x0E27, 0x0E27, 300, true},
    {"tpms_rl",    0x743, 0x0E28, 0x0E28, 300, true},
};
#define UDS_ENTRY_COUNT (sizeof(kQueries) / sizeof(kQueries[0]))

struct UdsRuntime {
    unsigned long dueMs;      // when this entry may be asked next
    uint8_t  failStreak;
};
static UdsRuntime kRt[UDS_ENTRY_COUNT];

static int8_t sInFlight = -1;              // index of outstanding query
static unsigned long sInFlightMs = 0;
static unsigned long sLastQueryMs = 0;
static unsigned long sLastActivitySampleMs = 0;
static uint32_t sLastFramesB = 0;
static uint32_t sLastFramesA = 0;
static bool sWindowOpen = false;
static unsigned long sWindowUntilMs = 0;
static bool sBootDone = false;

// ---- parsing ---------------------------------------------------------------

static int16_t be16(const uint8_t* d, uint8_t off) {
    return (int16_t)(((uint16_t)d[off] << 8) | d[off + 1]);
}

static void applyValue(const UdsEntry& e, const uint8_t* d, uint8_t len) {
    if (len < 5) return;
    const char* n = e.name;
    if (!strcmp(n, "gear")) {
        udsData.gear = (int8_t)d[4];
    } else if (!strcmp(n, "v12")) {
        udsData.v12 = (float)d[4] * 0.08f;
    } else if (!strcmp(n, "i12")) {
        if (len < 6) return;
        udsData.i12 = (float)be16(d, 4) / 256.0f;
    } else if (!strcmp(n, "plug")) {
        udsData.plug = (int8_t)d[4];
    } else if (!strcmp(n, "chargemode")) {
        udsData.chargeMode = (int8_t)d[4];
    } else if (!strcmp(n, "speed")) {
        if (len < 6) return;
        udsData.speed = (float)(uint16_t)be16(d, 4) / 10.0f;
    } else if (!strcmp(n, "obcpower")) {
        if (len < 6) return;
        udsData.obcW = (int)(uint16_t)be16(d, 4) * 100;
    } else if (!strcmp(n, "ac")) {
        udsData.acOn = d[4] ? 1 : 0;
    } else if (!strcmp(n, "eco")) {
        udsData.eco = (d[4] == 0x10 || d[4] == 0x11) ? 1 : 0;
    } else if (!strcmp(n, "epedal")) {
        udsData.epedal = (d[4] == 0x04) ? 1 : 0;
    } else if (!strcmp(n, "rearheater")) {
        udsData.rearHeater = (d[4] == 0xA2) ? 1 : 0;
    } else if (!strcmp(n, "motortemp")) {
        udsData.motorTempC = (float)d[4] - 40.0f;
    } else if (!strcmp(n, "brake")) {
        if (len < 6) return;
        udsData.brakeRaw = (uint16_t)be16(d, 4);
    } else if (!strcmp(n, "accel")) {
        udsData.accelPos = d[4];
    } else if (!strcmp(n, "swa")) {
        if (len < 6) return;
        udsData.swaDeg = (float)be16(d, 4) / 10.0f;
    } else if (!strcmp(n, "qccount")) {
        if (len < 6) return;
        udsData.qcCount = (uint16_t)be16(d, 4);
    } else if (!strcmp(n, "l1l2count")) {
        if (len < 6) return;
        udsData.l1l2Count = (uint16_t)be16(d, 4);
    } else if (!strncmp(n, "tpms_", 5)) {
        uint8_t idx = 0;
        if (!strcmp(n, "tpms_fr")) idx = 0;
        else if (!strcmp(n, "tpms_fl")) idx = 1;
        else if (!strcmp(n, "tpms_rr")) idx = 2;
        else idx = 3;
        // kPa = (raw * 0.068947576) * 100 / 4
        udsData.tpmsKpa[idx] = (float)d[4] * 0.068947576f * 100.0f / 4.0f;
        udsData.tpmsSeen |= (1 << idx);
    }
    udsData.lastOkMs = millis();
}

// ---- scheduler -------------------------------------------------------------

bool udsQueryBusy() { return sInFlight >= 0; }

static bool entryEnabled(const UdsEntry& e) {
    if (e.tpms && !getUdsTpmsEnabled()) return false;
    return true;
}

static uint32_t entryPeriodMs(const UdsEntry& e) {
    return (uint32_t)e.periodSec * 1000UL;
}

static bool udsDiagnosticWindow(unsigned long currentTimeMs) {
    extern uint32_t canFramesB;
    extern uint32_t canFramesA;
    extern bool isChargingNow;
    if (!sBootDone) {
        sLastFramesB = canFramesB;
        sLastFramesA = canFramesA;
        sLastActivitySampleMs = currentTimeMs;
        sBootDone = true;
    }
    if (currentTimeMs - sLastActivitySampleMs >= 5000) {
        uint32_t frameDelta = canFramesB - sLastFramesB;
        uint32_t evDelta = canFramesA - sLastFramesA;
        bool busActive = isChargingNow || frameDelta >= 20 || evDelta >= 100;
        if (busActive) sWindowUntilMs = currentTimeMs + 20000;
        sWindowOpen = busActive || currentTimeMs < sWindowUntilMs;
        sLastFramesB = canFramesB;
        sLastFramesA = canFramesA;
        sLastActivitySampleMs = currentTimeMs;
    }
    return sWindowOpen;
}

void manageUdsTier1(unsigned long currentTimeMs) {
#if LEAF_CAN_RX_ONLY
    (void)currentTimeMs;
    return;
#else
    if (!getUdsEnabled()) return;

    // In-flight: enforce the reply timeout, back the entry off on silence.
    if (sInFlight >= 0) {
        if (currentTimeMs - sInFlightMs > 1500) {
            UdsRuntime& rt = kRt[sInFlight];
            rt.failStreak++;
            uint32_t back = entryPeriodMs(kQueries[sInFlight]);
            if (rt.failStreak >= 3) back = 1800000UL;   // minutes of silence
            else if (rt.failStreak >= 2) back *= 4;
            rt.dueMs = currentTimeMs + back;
            sInFlight = -1;
        }
        return;
    }

    // Never interleave with command sequences or the ISO-TP pollers (BMS/VIN).
    if (activeSequence != CanSequence::NONE) return;
    if (diagChannelBusy()) return;
    if (!udsDiagnosticWindow(currentTimeMs)) return;
    if (sLastQueryMs != 0 && currentTimeMs - sLastQueryMs < 2000) return;

    int8_t pick = -1;
    for (uint8_t i = 0; i < UDS_ENTRY_COUNT; i++) {
        if (!entryEnabled(kQueries[i])) continue;
        if (kRt[i].dueMs == 0) {
            kRt[i].dueMs = currentTimeMs + 15000UL + (unsigned long)i * 3000UL;
            continue;
        }
        if ((long)(currentTimeMs - kRt[i].dueMs) >= 0) {
            if (pick < 0 || (long)(kRt[i].dueMs - kRt[pick].dueMs) < 0) {
                pick = (int8_t)i;
            }
        }
    }
    if (pick < 0) return;

    const UdsEntry& e = kQueries[pick];
    const uint8_t hi = (uint8_t)(e.did >> 8), lo = (uint8_t)(e.did & 0xFF);
    uint8_t req[8] = {0x03, 0x22, hi, lo, 0, 0, 0, 0};
    if (sendCAN(e.reqId, req, sizeof(req))) {
        sInFlight = pick;
        sInFlightMs = currentTimeMs;
        sLastQueryMs = currentTimeMs;
        udsData.reqCount++;
    } else {
        // Rejected by the whitelist or dropped: back this entry off.
        kRt[pick].dueMs = currentTimeMs + entryPeriodMs(e) * 4;
    }
#endif
}

// ---- response routing ------------------------------------------------------

bool udsFrameRouter(uint32_t id, const uint8_t* data, uint8_t len) {
    if (sInFlight < 0 || !data || len < 2) return false;
    const UdsEntry& e = kQueries[sInFlight];
    if (id != e.reqId + 3) return false;

    // NRC single frame: [len] 7F 22 <code> — ECU refuses this DID.
    if (data[0] <= 0x08 && len >= 4 && data[1] == 0x7F) {
        UdsRuntime& rt = kRt[sInFlight];
        rt.failStreak++;
        rt.dueMs = millis() + (rt.failStreak >= 2 ? 1800000UL : 120000UL);
        sInFlight = -1;
        return true;
    }
    // Single frame only: PCI 0x03/0x04, service echo 0x62, DID matches.
    if ((data[0] & 0xF0) != 0x00 || len < 4) return false;
    if (data[0] < 3 || data[0] > 8) return false;
    if (data[1] != 0x62) return false;
    if (data[2] != (uint8_t)(e.didResp >> 8) || data[3] != (uint8_t)(e.didResp & 0xFF)) {
        // Wrong DID echo: ignore the frame but keep the query in flight until
        // the timeout — some gateways relay stale answers.
        return false;
    }

    applyValue(e, data, len);
    kRt[sInFlight].failStreak = 0;
    kRt[sInFlight].dueMs = millis() + entryPeriodMs(e);
    sInFlight = -1;
    udsData.respCount++;
    return true;
}

// ---- TX whitelist ----------------------------------------------------------

bool udsWhitelistHit(uint32_t id, const uint8_t* data, uint8_t len) {
    if (!data || len != 8 || data[0] != 0x03 || data[1] != 0x22) return false;
    uint16_t did = ((uint16_t)data[2] << 8) | data[3];
    if (data[4] || data[5] || data[6] || data[7]) return false;
    for (uint8_t i = 0; i < UDS_ENTRY_COUNT; i++) {
        if (kQueries[i].reqId == id && kQueries[i].did == did) return true;
    }
    return false;
}

// ---- helpers ---------------------------------------------------------------

const char* udsGearText() {
    switch (udsData.gear) {
        case 1: return "P";
        case 2: return "R";
        case 3: return "N";
        case 4: return "D";
        case 7: return "Eco";
        default: return "";
    }
}

bool udsTpmsConfigured() { return getUdsTpmsEnabled(); }
