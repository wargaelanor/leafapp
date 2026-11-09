#ifndef UDS_LEAF_H
#define UDS_LEAF_H

#include <inttypes.h>
#include <stdbool.h>

// ===== Tier-1 UDS reads from Leaf2018-CAN.pdf ================================
// Single-frame ReadDataByIdentifier (service 0x22) requests. Every value has
// been picked because the broadcast frames do not carry it (gear, 12 V net,
// plug state, charge mode, session counters, VCM speed, OBC power) or because
// the broadcast can go silent while the car is awake.
//
// Polite by construction: one outstanding query, rotated deadlines, and only
// inside the same "diagnostic window" the BMS poller uses, so sleeping cars
// are never poked and the 12 V battery is never kept awake by us.

struct UdsTier1Data {
    int8_t gear = -1;          // 1=P 2=R 3=N 4=D 7=Eco, -1 unknown
    float  v12 = -1.0f;        // auxiliary battery voltage, V
    float  i12 = 999.0f;       // auxiliary battery current, A (+charge -drain)
    int8_t plug = -1;          // J1772: 0 not plugged, 1 partial, 2 plugged
    int8_t chargeMode = -1;    // 0 none, 1 L1, 2 L2, 3 QC
    float  speed = -1.0f;      // VCM speed km/h
    int    obcW = -1;          // on-board charger input power, W
    uint32_t qcCount = 0;      // quick-charge session counter
    uint32_t l1l2Count = 0;    // L1/L2 session counter
    int8_t eco = -1;
    int8_t epedal = -1;
    int8_t acOn = -1;
    int8_t rearHeater = -1;
    float  motorTempC = -99.0f;
    float  swaDeg = -999.0f;   // steering wheel angle, deg
    int    brakeRaw = -1;      // 0..369
    int    accelPos = -1;      // 0..200
    float  tpmsKpa[4] = {-1, -1, -1, -1};  // FR FL RR RL (DIDs 0E25..0E28)
    uint8_t tpmsSeen = 0;      // bit i set when tpmsKpa[i] valid
    unsigned long lastOkMs = 0;
    uint32_t reqCount = 0;
    uint32_t respCount = 0;
};

extern UdsTier1Data udsData;

void manageUdsTier1(unsigned long currentTimeMs);
bool udsQueryBusy();                       // for the BMS/VIN pollers
bool udsWhitelistHit(uint32_t id, const uint8_t* data, uint8_t len);
// Called from the CAN dispatch for every received frame; returns true when the
// frame was a Tier-1 UDS response and has been consumed.
bool udsFrameRouter(uint32_t id, const uint8_t* data, uint8_t len);

const char* udsGearText();                 // "P"/"R"/"N"/"D"/"Eco"/""
bool udsTpmsConfigured();                  // sensors installed (config flag)
// Bit i: ECU slot i (0 VCM, 1 BCM, 2 ABS, 3 TMI) has a live session / refused it.
uint8_t udsSessionMask();
uint8_t udsSessionRefusedMask();

#endif
