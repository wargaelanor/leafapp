#include "canLeafZE1.h"
#include "evCanLeaf.h"
#include <Arduino.h>
#include <cstring>

// Message parser dispatch
struct MessageParser {
    uint32_t id;
    uint8_t expected_len;
    void (*handler)(const uint8_t* data, uint8_t len);
};

// Helper macro to create parser entries from CANMessage objects
#define CAN_PARSER(msg, handler) { (msg).identifier, decltype(msg)::data_size, handler }

// Message-specific handlers
void handleSOCMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    // 0x55B is the BMS "nominal" SOC, not the dashboard value. Blank frames
    // (0xFF 0xC0 ...) decode above 100 %, so filter them out here.
    float soc = extractScaledValue(data, soc_field, soc_scaling);
    if (soc < 0.0f || soc > 100.0f) return;
    handleRawSOC(soc);
}

void handleTempMessage(const uint8_t* data, uint8_t len) {
    float temp = extractScaledValue(data, cabin_temp_field, cabin_temp_scaling);
    handleCabinTemp(temp);
}

void handleCarAwakeMessage(const uint8_t* data, uint8_t len) {
    handleCarAwake();
}

void handleChargerMessage(const uint8_t* data, uint8_t len) {
    uint8_t cStatus = static_cast<uint8_t>(extractBits(data, cStatus_field));
    bool qc_state = extractBool(data, qc_state_field);
    bool vg_state = (extractBits(data, vg_state_field) == 0x09);
    float chargeVoltage = extractScaledValue(data, chargeVolt_field, voltage_scaling);

    bool isCharging = false;
    ChargerState state = ChargerState::IDLE;

    switch (cStatus) {
        case 0x01:
            if (qc_state && !vg_state) { state = ChargerState::CHARGING; isCharging = true; }
            break;
        case 0x04:
            if (chargeVoltage > 0) { state = ChargerState::CHARGING; isCharging = true; }
            else { state = ChargerState::INTERRUPTED; }
            break;
        case 0x02:
            state = ChargerState::FINISHED;
            break;
        case 0x0c:
            state = ChargerState::WAITING;
            break;
        default:
            break;
    }

    handleChargerStatus(isCharging, state);
}

void handleHVACSetpointMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    float setpoint = extractScaledValue(data, hvac_setpoint_field, setpoint_scaling);
    handleHVACSetpoint(setpoint);
}

void handleAmbientTempMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    if (data[6] == 0xFF) return;
    float ambient = data[6] * 0.5f - 40.0f;
    if (ambient >= -40.0f && ambient <= 60.0f) handleEvAmbient(ambient);
}

VentilationMode decodeVentilationMode(uint8_t byte2) {
    // OVMS implementation: byte 2 of 0x54B message contains ventilation mode
    switch (byte2) {
        case 0x80: return VentilationMode::OFF;
        case 0x88: return VentilationMode::FACE;
        case 0x90: return VentilationMode::FACE_FEET;
        case 0x98: return VentilationMode::FEET;
        case 0xA0: return VentilationMode::WINDSCREEN_FEET;
        case 0xA8: return VentilationMode::WINDSCREEN;
        default: return VentilationMode::UNKNOWN;
    }
}

void handleHVACMessage(const uint8_t* data, uint8_t len) {
    // Extract new metrics
    bool heating = extractBool(data, hvac_bit0_field);
    bool cooling = extractBool(data, hvac_bit4_field) || extractBool(data, hvac_bit5_field);
    float fanSpeed = extractScaledValue(data, fan_speed_field, fan_speed_scaling);
    
    // Ventilation mode lives in byte 2 — only trust it when the frame is long enough
    VentilationMode ventMode = (len >= 3)
        ? decodeVentilationMode((uint8_t)extractBits(data, ventilation_mode_field))
        : VentilationMode::UNKNOWN;
    
    // HVAC enabled logic from OVMS
    bool isOn = (data[1] != 0x08 && data[1] != 0x04);
    
    handleHVACStatus(isOn);
    handleFanSpeed(fanSpeed);
    handleHeatingMode(heating, cooling);
    handleVentilationMode(ventMode);
}



void handleDoorsAndLocksMessage(const uint8_t* data, uint8_t len) {
    bool trunk = extractBool(data, door_trunk_field);
    bool rr = extractBool(data, door_rr_field);
    bool rl = extractBool(data, door_rl_field);
    bool fr = extractBool(data, door_fr_field);
    bool fl = extractBool(data, door_fl_field);
    bool locked = extractBool(data, locked_field);
    bool lightsOn = extractBool(data, headlight_dip_field) || extractBool(data, headlight_main_field);
    // OVMS derives the car state from 0x60D bits 1-2 of byte 1:
    // 0 = off, 1 = accessory, 2 = on, 3 = ready.
    uint8_t carState = (data[1] >> 1) & 0x03;

    handleDoorStatus(fl, fr, rl, rr, trunk);
    handleLockStatus(locked);
    handleHeadlightsStatus(lightsOn);
    if (carState == 3) handleEvVehicleOn(true);
    else if (carState == 0) handleEvVehicleOn(false);
    // OVMS stops repeating as soon as the requested lock state is observed.
    if ((activeSequence == CanSequence::LOCK_DOORS && locked) ||
        (activeSequence == CanSequence::UNLOCK_DOORS && !locked)) {
        activeSequence = CanSequence::NONE;
    }
}

void handleRangeInstrumentMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    // ZE1 dashboard range confirmed on this car: 05 10 -> 162 km while the
    // cluster displayed 162 km. The OVMS /5 formula is incorrect for ZE1.
    uint16_t raw = ((uint16_t)(data[1] & 0x7F) << 5) | (data[2] >> 3);
    extern uint16_t lastRangeRawA9;
    if (raw != lastRangeRawA9) {
        lastRangeRawA9 = raw;
        Serial.printf("0x5A9 dashboard range=%u km\n", raw);
    }
    if (raw > 0 && raw < 1000) handleInstrumentRange(raw);
}

uint16_t lastRangeRawA9 = 0;

// 0x5BC — HV battery capacity: GIDS + SOH. Normally an EV-CAN frame, but the
// ZE1 also broadcasts it on IT-CAN (Port B), which is our working bus.
// Verified on car: 3D 80 ... -> GIDS 246 at 61 % dashboard, A1 -> SOH 80 %.
void handleBatteryCapacityMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    static int lastGids = -1;
    static int lastSoh = -1;
    // IT-CAN alternates valid relayed GIDS with a 0x7D00 service variant.
    // 0x7D00 decodes to a fake 500 GIDS and made range jump to ~300 km.
    // Do not filter bit6 generally: valid ~80% values around 320 GIDS use it.
    bool gidsFrame = !(data[0] == 0x7D && (data[1] & 0xC0) == 0x00);
    int gids = (data[0] << 2) | (data[1] >> 6);
    int soh  = (data[4] >> 1) & 0x7F;
    if (gidsFrame && gids > 0 && gids <= 500 && gids != lastGids) {
        lastGids = gids;
        handleEvGids(gids);
    }
    if (soh > 0 && soh <= 100 && soh != lastSoh) {
        lastSoh = soh;
        handleEvSOH(soh);
    }
}

// 0x5C0 — LBC pack temperature. The temperature is only carried by the
// multiplexed mux 1/2/3 frames (OVMS: d[0] >> 6 == 1, temp = d[2]/2 - 40).
// Every Port B log so far only had mux 0, which carries no temperature.
void handleBatteryTempHvbMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    uint8_t mux = (data[0] >> 6) & 0x03;
    if (mux == 0) return;
    float temp = data[2] * 0.5f - 40.0f;
    if (temp >= -40.0f && temp <= 80.0f) handleBatteryTemp(temp);
}

void handlePackStatusMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    int voltRaw = (data[2] << 2) | (data[3] >> 6);
    int curRaw = (data[0] << 3) | (data[1] >> 5);
    if (curRaw & 0x400) curRaw -= 0x800;
    float voltage = voltRaw * 0.5f;
    float current = curRaw * -0.5f;
    if (voltage >= 200.0f && voltage <= 500.0f) handleEvPackStatus(voltage, current);
}

// 0x5B3 — VCM pack temperature, 0.25 C/bit. Present on IT-CAN in every log,
// used as a fallback while 0x5C0 carries no temperature mux.
void handleBatteryTempVcmMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    if (data[0] == 0xFF) return; // blank frame (63.75 C is not a reading)
    float temp = data[0] * 0.25f;
    if (temp >= -40.0f && temp <= 80.0f) handleBatteryTemp(temp);
}

static int8_t odometerUnits = -1; // 0 km, 1 miles

void handleOdometerUnitsMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    odometerUnits = (data[6] & 0x20) ? 1 : 0;
}

void handleOdometerMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    if (odometerUnits < 0) return;
    uint32_t raw = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    uint32_t km = odometerUnits == 1 ? (uint32_t)(raw * 1.609344f + 0.5f) : raw;
    if (km > 0 && km < 2000000UL) handleOdometer(km);
}

static char vinBuffer[18] = {0};
static uint8_t vinLength = 0;
static uint8_t vinNextFrame = 1;
static bool vinPending = false;
static unsigned long lastVinRequestMs = 0;
static uint32_t vinRequestCount = 0;
static uint32_t vinResponseCount = 0;
static uint8_t vinInvalidCount = 0;
static bool vinDisabled = false;
// Optional read-only VIN probe over standard OBD-II mode 09 PID 02. The LBC on
// this car answers the UDS request (02 21 81 on 0x797) with 17 blanks, so the
// VCM through the OBD-II functional address is the only remaining source.
// It runs only when explicitly requested from the developer page, which keeps
// the default TX whitelist limited to the payloads OVMS proves for this car.
static bool vinObdRequest = false;
static bool vinObdPending = false;
static unsigned long vinObdStartedMs = 0;
static unsigned long vinObdLastRequestMs = 0;
static uint8_t vinObdAttempts = 0;
static char vinObdBuffer[18] = {0};
static uint8_t vinObdLength = 0;
static uint8_t vinObdNextFrame = 1;
static uint32_t bmsRequestCount = 0;
static uint32_t bmsResponseCount = 0;
static uint16_t bmsLastExpected = 0;
static uint8_t bmsLastGroup = 0;
static uint32_t bmsFirstFrameCount = 0;

enum class BmsQuery : uint8_t { NONE, CELLS, TEMPERATURES, BATTERY, SOH };
static BmsQuery bmsQuery = BmsQuery::NONE;
static uint8_t bmsIsoTp[340];   // group 0x61 (SOH) is 329 bytes
static uint16_t bmsExpectedLength = 0;
static uint16_t bmsReceivedLength = 0;
static uint8_t bmsNextFrame = 1;
static unsigned long bmsQueryStartedMs = 0;
static unsigned long lastBmsQueryMs = 0;
// OVMS polls four ZE1 groups: 0x01 battery (SOC/Hx/Ah), 0x02 cells,
// 0x04 temperatures, 0x61 SOH. Rotating through all of them gives the same
// data set as upstream instead of only cells/temperatures.
static const uint8_t kBmsGroups[] = {0x04, 0x01, 0x61};
static const BmsQuery kBmsQueryTypes[] = {
    BmsQuery::TEMPERATURES, BmsQuery::BATTERY, BmsQuery::SOH
};
#define BMS_GROUP_COUNT (sizeof(kBmsGroups) / sizeof(kBmsGroups[0]))
static uint8_t bmsGroupIndex = 0;
static uint8_t bmsRequestedGroup = 0;
static uint8_t bmsFailStreak = 0;
// Per-group cadence. Upstream OVMS asks cells and battery every 60 s,
// temperatures every 180 s and SOH every 900 s. Asking every 2.5 s made this
// LBC answer two groups and then ignore everything for minutes, which looked
// like "temperature never shows up". Keep a per-group slot and back a silent
// group off instead of flooding the gateway.
static const uint16_t kBmsPeriodSec[] = {180, 60, 900};
static const uint16_t kBmsBackoffMaxSec = 240;
static unsigned long bmsGroupDueMs[BMS_GROUP_COUNT] = {0, 0, 0};
static uint16_t bmsGroupBackoffSec[BMS_GROUP_COUNT] = {0, 0, 0};
static unsigned long bmsLastReplyMs = 0;
static uint32_t bmsFailCount = 0;
// Measured on the car: the LBC answers two groups right after the car wakes up
// and then ignores the gateway for minutes no matter how slowly the next
// request comes. That is a cooldown on the diagnostic channel, not a payload
// problem, so after repeated silence we stop polling entirely for a while and
// let the channel recover instead of piling more requests on it.
static unsigned long bmsCooldownUntilMs = 0;
static const unsigned long kBmsCooldownMs = 180000UL;
static const uint8_t kBmsFailStreakLimit = 2;
// Set by bmsRequestImmediateBurst(): keep the poll window open without waiting
// for the next activity sample.
static unsigned long bmsForceWindowUntilMs = 0;

// Schedule the group that just finished: its normal period when it answered,
// an exponential backoff when it did not.
static void bmsGroupDone(bool answered) {
    uint8_t index = bmsGroupIndex;
    if (index >= BMS_GROUP_COUNT) return;
    if (answered) {
        bmsFailStreak = 0;
        bmsGroupBackoffSec[index] = 0;
        bmsGroupDueMs[index] = millis() + (unsigned long)kBmsPeriodSec[index] * 1000UL;
        bmsLastReplyMs = millis();
    } else {
        uint16_t backoff = bmsGroupBackoffSec[index]
                               ? (uint16_t)(bmsGroupBackoffSec[index] * 2)
                               : 20;
        if (backoff > kBmsBackoffMaxSec) backoff = kBmsBackoffMaxSec;
        bmsGroupBackoffSec[index] = backoff;
        bmsGroupDueMs[index] = millis() + (unsigned long)backoff * 1000UL;
        // Silence across several groups means the diagnostic channel is shut.
        // Give it minutes instead of adding requests it will not answer.
        if (bmsFailStreak >= kBmsFailStreakLimit) {
            bmsCooldownUntilMs = millis() + kBmsCooldownMs;
            bmsFailStreak = 0;
            for (uint8_t i = 0; i < BMS_GROUP_COUNT; i++) {
                bmsGroupDueMs[i] = bmsCooldownUntilMs;
            }
        }
    }
}

static void advanceBmsGroup() {
    bmsGroupDone(true);
    bmsGroupIndex = (uint8_t)((bmsGroupIndex + 1) % BMS_GROUP_COUNT);
    bmsFailStreak = 0;
}

static void finishBmsQuery() {
    bmsQuery = BmsQuery::NONE;
    bmsExpectedLength = 0;
    bmsReceivedLength = 0;
}

static void decodeBmsReply() {
    bmsLastGroup = bmsExpectedLength >= 2 ? bmsIsoTp[1] : 0;
    if (bmsExpectedLength < 2 || bmsReceivedLength < bmsExpectedLength ||
        bmsIsoTp[0] != 0x61) {
        finishBmsQuery();
        return;
    }
    uint8_t group = bmsIsoTp[1];
    const uint8_t* payload = bmsIsoTp + 2;
    uint16_t payloadLength = bmsExpectedLength - 2;

    if (bmsQuery == BmsQuery::CELLS && group == 0x02 && payloadLength >= 192) {
        // Cell voltages are 16-bit pairs. Big-endian is the documented layout,
        // but clone LBC firmware has been seen packing them little-endian, so
        // accept whichever order yields 96 plausible cells.
        uint16_t minMv = 0, maxMv = 0;
        uint8_t validCells = 0;
        for (uint8_t pass = 0; pass < 2; pass++) {
            bool little = (pass == 1);
            minMv = UINT16_MAX;
            maxMv = 0;
            validCells = 0;
            for (uint8_t i = 0; i < 96; i++) {
                uint16_t lo = payload[i * 2];
                uint16_t hi = payload[i * 2 + 1];
                uint16_t mv = little ? (uint16_t)(lo | (hi << 8))
                                     : (uint16_t)((lo << 8) | hi);
                if (mv >= 2000 && mv < 5000) {
                    minMv = min(minMv, mv);
                    maxMv = max(maxMv, mv);
                    validCells++;
                }
            }
            if (validCells == 96) break;
        }
        if (validCells == 96) handleCellDelta((int)maxMv - (int)minMv);
        if (payloadLength >= 194) {
            uint16_t packCentivolts = ((uint16_t)payload[192] << 8) | payload[193];
            handlePackVoltage(packCentivolts / 100.0f);
        }
        advanceBmsGroup();
    } else if (bmsQuery == BmsQuery::TEMPERATURES && group == 0x04 &&
               (payloadLength == 14 || payloadLength == 29)) {
        float hottest = -99.0f;
        for (uint8_t i = 0; i < 4; i++) {
            uint16_t be = ((uint16_t)payload[i * 3] << 8) | payload[i * 3 + 1];
            uint16_t le = (uint16_t)(payload[i * 3] | (payload[i * 3 + 1] << 8));
            for (uint8_t pass = 0; pass < 2; pass++) {
                uint16_t thermistor = pass == 0 ? be : le;
                if (thermistor == 0xFFFF || thermistor == 0) continue;
                float temp = -0.102f * ((int)thermistor - 710);
                if (temp >= -40.0f && temp <= 80.0f) {
                    hottest = max(hottest, temp);
                    break;
                }
            }
        }
        if (hottest > -99.0f) handleBatteryTemp(hottest);
        advanceBmsGroup();
    } else if (bmsQuery == BmsQuery::BATTERY && group == 0x01 && payloadLength >= 51) {
        // ZE1 51-byte reply: Hx [28..29]/102.4, SOC [31..33]/10000,
        // usable Ah [35..37]/10000 (matches OVMS PollReply_Battery).
        uint32_t socRaw = ((uint32_t)payload[31] << 16) |
                          ((uint32_t)payload[32] << 8) | payload[33];
        float soc = socRaw / 10000.0f;
        if (soc > 0.0f && soc <= 100.0f) handleEvBmsSOC(soc);
        advanceBmsGroup();
    } else if (bmsQuery == BmsQuery::SOH && group == 0x61 && payloadLength >= 4) {
        uint16_t raw = ((uint16_t)payload[2] << 8) | payload[3];
        int soh = (int)(raw / 100);
        if (soh > 0 && soh <= 100) handleEvSOH(soh);
        advanceBmsGroup();
    }
    finishBmsQuery();
}

void handleBmsResponse(const uint8_t* data, uint8_t len) {
    if (bmsQuery == BmsQuery::NONE || len < 2) return;
    bmsResponseCount++;
    uint8_t frameType = data[0] & 0xF0;
    if (frameType == 0x10) {
        uint16_t total = ((uint16_t)(data[0] & 0x0F) << 8) | data[1];
        bmsLastExpected = total;
        bmsFirstFrameCount++;
        uint8_t expectedGroup = bmsRequestedGroup;
        if (total > sizeof(bmsIsoTp) || len < 4 || data[2] != 0x61 || data[3] != expectedGroup) {
            finishBmsQuery();
            return;
        }
        bmsExpectedLength = total;
        bmsReceivedLength = 0;
        bmsNextFrame = 1;
        for (uint8_t i = 2; i < len && bmsReceivedLength < total; i++)
            bmsIsoTp[bmsReceivedLength++] = data[i];
        // Plain flow control: some LBC firmware aborts when STmin is not 0.
        // The dedicated drain in manageBmsDiagnostics keeps the frames from
        // being lost to Wi-Fi work instead.
        const uint8_t flowControl[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
        if (!sendCAN(0x79B, flowControl, sizeof(flowControl))) finishBmsQuery();
    } else if (frameType == 0x20 && (data[0] & 0x0F) == (bmsNextFrame & 0x0F)) {
        bmsNextFrame++;
        for (uint8_t i = 1; i < len && bmsReceivedLength < bmsExpectedLength; i++)
            bmsIsoTp[bmsReceivedLength++] = data[i];
        if (bmsReceivedLength >= bmsExpectedLength) decodeBmsReply();
    } else {
        // A missing consecutive frame breaks the sequence; keeping the state
        // around only wastes the whole reply, so drop it and retry later.
        finishBmsQuery();
    }
}

void handleVinResponse(const uint8_t* data, uint8_t len) {
    if (!vinPending || len < 2) return;
    vinResponseCount++;
    if ((data[0] & 0xF0) == 0x10 && len >= 5 && data[2] == 0x61 && data[3] == 0x81) {
        vinLength = 0;
        vinNextFrame = 1;
        for (uint8_t i = 4; i < len && vinLength < 17; i++) vinBuffer[vinLength++] = (char)data[i];
        const uint8_t flowControl[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
        sendCAN(0x797, flowControl, sizeof(flowControl));
    } else if ((data[0] & 0xF0) == 0x20 && (data[0] & 0x0F) == (vinNextFrame & 0x0F)) {
        vinNextFrame++;
        for (uint8_t i = 1; i < len && vinLength < 17; i++) vinBuffer[vinLength++] = (char)data[i];
        if (vinLength == 17) {
            vinBuffer[17] = '\0';
            vinPending = false;
            // A real VIN is 17 alphanumerics without I/O/Q. Anything else is a
            // stray frame on 0x79A (we have seen zero-filled ones); reject it
            // and let the next poll try again.
            bool valid = true;
            for (uint8_t i = 0; i < 17; i++) {
                char c = vinBuffer[i];
                bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
                if (!alnum || c == 'I' || c == 'O' || c == 'Q') { valid = false; break; }
            }
            if (valid) {
                handleVin(vinBuffer);
            } else {
                vinBuffer[0] = '\0';
                vinLength = 0;
                lastVinRequestMs = 0;
                // The car answers with blanks/spaces instead of a VIN. Retrying
                // without a cooldown flooded 0x797 and stole time from the BMS
                // polls, so give up after a few honest attempts.
                if (++vinInvalidCount >= 3) {
                    vinDisabled = true;
                    Serial.println("VIN: car returns no usable VIN, polling disabled");
                } else {
                    lastVinRequestMs = millis() - 25000;
                }
            }
        }
    }
}

// Message parser dispatch table
void handleVinObdResponse(const uint8_t* data, uint8_t len);
static const MessageParser parsers[] = {
    CAN_PARSER(raw_soc_readout, handleSOCMessage),
    CAN_PARSER(cabin_temp_readout, handleTempMessage),
    CAN_PARSER(car_awake_readout, handleCarAwakeMessage),
    CAN_PARSER(charger_status_readout, handleChargerMessage),
    CAN_PARSER(hvac_status_readout, handleHVACMessage),
    CAN_PARSER(hvac_setpoint_readout, handleHVACSetpointMessage),
    CAN_PARSER(ambient_temp_readout, handleAmbientTempMessage),
    CAN_PARSER(doors_and_locks_readout, handleDoorsAndLocksMessage),
    CAN_PARSER(range_instrument_readout, handleRangeInstrumentMessage),
    CAN_PARSER(battery_capacity_readout, handleBatteryCapacityMessage),
    CAN_PARSER(battery_temp_hvb_readout, handleBatteryTempHvbMessage),
    CAN_PARSER(battery_pack_readout, handlePackStatusMessage),
    // 0x5B3 byte0 / 4 produced 20 C while LeafSpy battery sensors were
    // 9.6-11.4 C on this ZE1. Do not publish that unverified fallback.
    CAN_PARSER(odometer_units_readout, handleOdometerUnitsMessage),
    CAN_PARSER(odometer_readout, handleOdometerMessage),
    { 0x79A, 1, handleVinResponse },
    { 0x7BB, 1, handleBmsResponse },
    { 0x7E8, 1, handleVinObdResponse },   // only used by the explicit VIN probe
};

// Can message reception handler, to be called when a CAN message is received
// Last raw payload per diagnostic-relevant ID, for remote format debugging.
struct DbgFrame {
    uint32_t id;
    uint8_t len;
    bool seen;
    uint8_t data[8];
};
static DbgFrame dbgFrames[] = {
    {0x1DB, 0, false, {0}},
    {0x5BC, 0, false, {0}},
    {0x7BB, 0, false, {0}},
    {0x79A, 0, false, {0}},
};

uint8_t dbgLastFrame(uint32_t id, uint8_t* out) {
    for (auto& f : dbgFrames) {
        if (f.id == id && f.seen) {
            if (out) memcpy(out, f.data, sizeof(f.data));
            return f.len;
        }
    }
    return 0;
}

void handleReceivedMessage(uint32_t identifier, uint8_t* data, uint8_t len) {
    for (auto& f : dbgFrames) {
        if (f.id == identifier && data) {
            f.len = len > 8 ? 8 : len;
            memcpy(f.data, data, f.len);
            f.seen = true;
            break;
        }
    }
    for (const auto& p : parsers) {
        if (p.id == identifier && len >= p.expected_len) {
            p.handler(data, len);
            return;
        }
    }
}

// Sequence State Machine
extern bool carIsAwake;
CanSequence activeSequence = CanSequence::NONE;

enum class SeqPhase {
    IDLE,
    WAKING,
    WAKE_WAIT,
    RUN_STEP
};

static SeqPhase currentPhase = SeqPhase::IDLE;
static int wakeAttempts = 0;
static int stepIndex = 0;
static int repeatCount = 0;
static int sendFailCount = 0;
static unsigned long nextCANEventTime = 0;

struct SequenceStep {
    uint32_t id;
    const uint8_t* data;
    uint8_t len;
    int repeats;
    int intervalMs;
    int delayAfterMs;
    SequencePayloadModifier modifier;
    // OEM TCU cadence: `burstSize` frames at `intervalMs`, then pause
    // `burstPauseMs`. 0 = plain fixed-interval repeat.
    uint8_t burstSize;
    int burstPauseMs;
};

#define MSG_STEP(msg, reps, interv) { msg.identifier, msg.data, sizeof(msg.data), reps, interv, 0, nullptr }
#define MSG_DLY_STEP(msg, reps, interv, delay) { msg.identifier, msg.data, sizeof(msg.data), reps, interv, delay, nullptr }
#define MSG_MOD_STEP(msg, reps, interv, mod_fn) { msg.identifier, msg.data, sizeof(msg.data), reps, interv, 0, mod_fn }
#define MSG_WAIT_STEP(wait_ms) { 0, nullptr, 0, 1, wait_ms, wait_ms, nullptr }
// A burst step mimics the OEM TCU: 3 frames 100 ms apart, then ~800 ms pause.
#define MSG_BURST_STEP(msg, reps, interv, burst, pause) { msg.identifier, msg.data, sizeof(msg.data), reps, interv, 0, nullptr, burst, pause }

static uint8_t internal_hvac_setpoint_byte = 0;

void setHVACTargetTemperature(float setpoint) {
    internal_hvac_setpoint_byte = (setpoint >= 16.0f && setpoint <= 32.0f)
        ? static_cast<uint8_t>(setpoint * 2.0f)
        : 0;
    Serial.printf("HVAC setpoint request: %.1f C (command byte %u)\n",
                  setpoint, internal_hvac_setpoint_byte);
}

static void hvacPayloadModifier(uint8_t* data, uint8_t len) {
    if (len >= 3) data[2] = internal_hvac_setpoint_byte;
}

// Wake the TCU and leave it awake: ending a refresh with 86 00 00 00 (sleep)
// cut the data stream off right after the wake-up.
static const SequenceStep seq_refresh[] = {
    MSG_BURST_STEP(hvac_init, 6, 100, 3, 800),
    MSG_STEP(hvac_init, 4, 400)
};

// This car previously started climate reliably with the 0x12 enable and 0x32
// AUTO_DISABLE close payloads. Restore that proven command, but keep the wake
// and release tails short so the cluster self-test/parking-brake lamp does not
// keep blinking as it did with the old 136-frame sequence.
static const SequenceStep seq_hvac_on[] = {
    MSG_STEP(hvac_on_legacy, 24, 100)
};

// Remote climate OFF: 0x54B flips to "off" on the first 56 08 0A frame, the
// rest is the OEM release tail.
static const SequenceStep seq_hvac_off[] = {
    MSG_STEP(hvac_off_ovms, 24, 100)
};

static const SequenceStep seq_charge_on[] = {
    MSG_STEP(start_charge_data, 24, 100)
};

// Experimental: withdraw the remote-charge request (RC bit cleared via init
// frames) after an interrupt. No captured frame for stop exists yet — needs
// in-car verification.
static const SequenceStep seq_charge_off[] = {
    MSG_STEP(interrupt_data, 9, 100),
    MSG_STEP(hvac_init, 6, 100),
    MSG_STEP(idle_data, 3, 100)
};

// Clear RHL request (flash/horn off) — init frames carry RHL=0
static const SequenceStep seq_rhl_off[] = {
    MSG_BURST_STEP(hvac_init, 6, 100, 3, 800),
    MSG_STEP(idle_data, 3, 100)
};

// Unlock: OEM sets bit0 of byte0 (46 -> 47). In the capture the two request
// frames are ~800 ms apart (not 100 ms), then the TCU returns to standby.
static const SequenceStep seq_unlock_doors[] = {
    MSG_STEP(unlock_doors_data, 24, 100)
};

// Lock: OEM sends exactly two 46 80 frames 100 ms apart, then standby.
// The old 24x "60 80" burst was seen by the BCM as repeated lock events and
// made the actuators click 8-10 times without latching.
static const SequenceStep seq_lock_doors[] = {
    MSG_STEP(lock_doors_data, 24, 100)
};

static const SequenceStep seq_lights[] = {
    // Short, bounded flash. hvac_init clears RHL afterwards without adding an
    // unverified sleep/idle payload to the final TX whitelist.
    MSG_STEP(lights_data, 6, 100),
    MSG_STEP(hvac_init, 3, 100)
};

static const SequenceStep seq_horn_lights[] = {
    MSG_BURST_STEP(hvac_init, 6, 100, 3, 800),
    MSG_STEP(horn_light_data, 20, 100),
    MSG_STEP(hvac_init, 4, 100),
    MSG_STEP(idle_data, 3, 100)
};

struct SequenceDef {
    CanSequence seq;
    const SequenceStep* steps;
    int length;
};

static const SequenceDef sequence_map[] = {
    { CanSequence::REFRESH, seq_refresh, sizeof(seq_refresh)/sizeof(seq_refresh[0]) },
    { CanSequence::HVAC_ON, seq_hvac_on, sizeof(seq_hvac_on)/sizeof(seq_hvac_on[0]) },
    { CanSequence::HVAC_OFF, seq_hvac_off, sizeof(seq_hvac_off)/sizeof(seq_hvac_off[0]) },
    { CanSequence::CHARGE_ON, seq_charge_on, sizeof(seq_charge_on)/sizeof(seq_charge_on[0]) },
    { CanSequence::CHARGE_OFF, seq_charge_off, sizeof(seq_charge_off)/sizeof(seq_charge_off[0]) },
    { CanSequence::UNLOCK_DOORS, seq_unlock_doors, sizeof(seq_unlock_doors)/sizeof(seq_unlock_doors[0]) },
    { CanSequence::LOCK_DOORS, seq_lock_doors, sizeof(seq_lock_doors)/sizeof(seq_lock_doors[0]) },
    { CanSequence::LIGHTS, seq_lights, sizeof(seq_lights)/sizeof(seq_lights[0]) },
    { CanSequence::HORN_LIGHTS, seq_horn_lights, sizeof(seq_horn_lights)/sizeof(seq_horn_lights[0]) },
    { CanSequence::RHL_OFF, seq_rhl_off, sizeof(seq_rhl_off)/sizeof(seq_rhl_off[0]) }
};

bool startSequence(CanSequence seq, unsigned long currentTimeMs) {
#if LEAF_CAN_RX_ONLY
    (void)seq;
    (void)currentTimeMs;
    return false;
#else
    if (seq == CanSequence::NONE) return false;
    if (seq != CanSequence::REFRESH &&
        seq != CanSequence::HVAC_ON && seq != CanSequence::HVAC_OFF &&
        seq != CanSequence::CHARGE_ON && seq != CanSequence::UNLOCK_DOORS &&
        seq != CanSequence::LOCK_DOORS && seq != CanSequence::LIGHTS) {
        Serial.println("### Command blocked: not enabled in OVMS command build ###");
        return false;
    }
    if (activeSequence != CanSequence::NONE) return false; // busy: reject, report to caller
    resetCanLogTimestamps();
    
    activeSequence = seq;
    currentPhase = SeqPhase::WAKING;
    wakeAttempts = 0;
    stepIndex = 0;
    repeatCount = 0;
    sendFailCount = 0;
    carIsAwake = false;
    nextCANEventTime = currentTimeMs;
    return true;
#endif
}

CanSeqResult manageCANSequence(unsigned long currentTimeMs) {
    if (activeSequence == CanSequence::NONE) return CanSeqResult::IDLE;
    if ((long)(currentTimeMs - nextCANEventTime) < 0) return CanSeqResult::PROCESSING;

    switch (currentPhase) {
        case SeqPhase::WAKING:
            // The car's bus is often asleep: 0x68C wake pings alone do not
            // prove the car is awake, and OVMS simply follows its wake frames
            // with 24 repeats of the actual command (RemoteCommandTimer).
            // So after a short wake burst we send the command regardless —
            // the vehicle wakes up and accepts one of the repeats.
            if (!sendCAN(wakeup_data)) {
                activeSequence = CanSequence::NONE;
                currentPhase = SeqPhase::IDLE;
                return CanSeqResult::SEQUENCE_FINISHED;
            }
            wakeAttempts = 1;
            currentPhase = SeqPhase::WAKE_WAIT;
            nextCANEventTime = currentTimeMs + 50;
            break;
            
        case SeqPhase::WAKE_WAIT:
            if (!sendCAN(hvac_init)) {
                activeSequence = CanSequence::NONE;
                currentPhase = SeqPhase::IDLE;
                return CanSeqResult::SEQUENCE_FINISHED;
            }
            // Continue into seq_refresh as well. A single OVMS wake pair is
            // sufficient before a command burst, but this car needs the
            // retained 0x56E keepalive sequence to stay awake for telemetry.
            currentPhase = SeqPhase::RUN_STEP;
            stepIndex = 0;
            repeatCount = 0;
            nextCANEventTime = currentTimeMs;
            break;
            
        case SeqPhase::RUN_STEP: {
            bool done = false;
            const SequenceStep* currentSeqArray = nullptr;
            int currentSeqLen = 0;

            for (const auto& def : sequence_map) {
                if (def.seq == activeSequence) {
                    currentSeqArray = def.steps;
                    currentSeqLen = def.length;
                    break;
                }
            }

            if (!currentSeqArray) {
                done = true;
            }

            if (!done && stepIndex < currentSeqLen && currentSeqArray != nullptr) {
                const SequenceStep& step = currentSeqArray[stepIndex];
                
                if (step.id != 0 && step.data != nullptr) {
                    uint8_t buffer[8];
                    uint8_t safeLen = step.len > sizeof(buffer) ? sizeof(buffer) : step.len;
                    memcpy(buffer, step.data, safeLen);
                    if (step.modifier) {
                        step.modifier(buffer, safeLen);
                    }
                    if (!sendCAN(step.id, buffer, safeLen)) {
                        // Frame dropped (bus-off / TX full): retry the same step
                        // instead of silently advancing. Give up after 50 tries.
                        if (++sendFailCount >= 50) {
                            Serial.println("### Sequence aborted: CAN TX failing ###");
                            activeSequence = CanSequence::NONE;
                            currentPhase = SeqPhase::IDLE;
                            return CanSeqResult::SEQUENCE_FINISHED;
                        }
                        nextCANEventTime = currentTimeMs + 200;
                        break;
                    }
                }
                sendFailCount = 0;
                
                if (++repeatCount >= step.repeats) {
                    stepIndex++;
                    repeatCount = 0;
                    const uint32_t nextDelayMs =
                        (step.delayAfterMs > step.intervalMs) ? step.delayAfterMs : step.intervalMs;
                    nextCANEventTime = currentTimeMs + nextDelayMs;
                } else if (step.burstSize > 0 && (repeatCount % step.burstSize) == 0 &&
                           step.burstPauseMs > step.intervalMs) {
                    // OEM cadence: end of a 3-frame burst -> longer pause
                    nextCANEventTime = currentTimeMs + step.burstPauseMs;
                } else {
                    nextCANEventTime = currentTimeMs + step.intervalMs;
                }
            } else {
                done = true;
            }

            if (done) {
                activeSequence = CanSequence::NONE;
                currentPhase = SeqPhase::IDLE;
                return CanSeqResult::SEQUENCE_FINISHED;
            }
            break;
        }
    }
    return CanSeqResult::PROCESSING;
}

void manageVinQuery(unsigned long currentTimeMs) {
#if !LEAF_CAN_RX_ONLY
    extern uint32_t canFramesB;
    // A VIN reply that never reaches 17 bytes used to leave vinPending set
    // forever, which blocked every BMS diagnostic query after it.
    if (vinPending && currentTimeMs - lastVinRequestMs > 5000) vinPending = false;
    if (vinDisabled || vinBuffer[0] != '\0' || canFramesB == 0 ||
        activeSequence != CanSequence::NONE) return;
    if (vinPending && currentTimeMs - lastVinRequestMs < 5000) return;
    if (lastVinRequestMs != 0 && currentTimeMs - lastVinRequestMs < 30000) return;
    const uint8_t request[8] = {0x02, 0x21, 0x81, 0, 0, 0, 0, 0};
    if (sendCAN(0x797, request, sizeof(request))) {
        vinPending = true;
        vinRequestCount++;
        lastVinRequestMs = currentTimeMs;
        Serial.println("VIN request sent on IT-CAN");
    }
#else
    (void)currentTimeMs;
#endif
}

bool vinObdProbeActive() { return vinObdRequest; }

void requestVinObdProbe() {
    vinObdRequest = true;
    vinObdPending = false;
    vinObdAttempts = 0;
    vinObdStartedMs = 0;
    vinObdLastRequestMs = 0;
    vinObdLength = 0;
    vinObdBuffer[0] = '\0';
    Serial.println("VIN probe: OBD-II 09 02 on 0x7DF requested");
}

void manageVinObdProbe(unsigned long currentTimeMs) {
#if LEAF_CAN_RX_ONLY
    (void)currentTimeMs;
#else
    if (!vinObdRequest) return;
    if (vinObdPending && currentTimeMs - vinObdStartedMs > 3000) {
        vinObdPending = false;
    }
    if (vinObdAttempts >= 3 && !vinObdPending) {
        vinObdRequest = false;
        Serial.println("VIN probe: no reply to 09 02, giving up");
        return;
    }
    if (vinObdPending) return;
    if (vinObdLastRequestMs != 0 && currentTimeMs - vinObdLastRequestMs < 5000) return;
    const uint8_t request[8] = {0x02, 0x09, 0x02, 0, 0, 0, 0, 0};
    if (sendCAN(0x7DF, request, sizeof(request))) {
        vinObdPending = true;
        vinObdStartedMs = currentTimeMs;
        vinObdLastRequestMs = currentTimeMs;
        vinObdAttempts++;
        Serial.println("VIN probe: 09 02 sent on OBD CAN");
    } else {
        vinObdRequest = false;
        vinObdPending = false;
        Serial.println("VIN probe: CAN TX rejected, aborted");
    }
#endif
}

// ISO-TP reply to mode 09 PID 02: 49 02 01 followed by 17 VIN characters.
void handleVinObdResponse(const uint8_t* data, uint8_t len) {
    if (!vinObdRequest || !vinObdPending || len < 2) return;
    uint8_t frameType = data[0] & 0xF0;
    if (frameType == 0x10) {
        uint16_t total = ((uint16_t)(data[0] & 0x0F) << 8) | data[1];
        if (len < 6 || total < 20 || data[2] != 0x49 || data[3] != 0x02) return;
        vinObdLength = 0;
        vinObdNextFrame = 1;
        for (uint8_t i = 5; i < len && vinObdLength < 17; i++) {
            vinObdBuffer[vinObdLength++] = (char)data[i];
        }
        const uint8_t flowControl[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
        sendCAN(0x7E0, flowControl, sizeof(flowControl));
    } else if (frameType == 0x20 && (data[0] & 0x0F) == (vinObdNextFrame & 0x0F)) {
        vinObdNextFrame++;
        for (uint8_t i = 1; i < len && vinObdLength < 17; i++) {
            vinObdBuffer[vinObdLength++] = (char)data[i];
        }
        if (vinObdLength >= 17) {
            vinObdBuffer[17] = '\0';
            vinObdRequest = false;
            vinObdPending = false;
            bool valid = true;
            for (uint8_t i = 0; i < 17; i++) {
                char c = vinObdBuffer[i];
                bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
                if (!alnum || c == 'I' || c == 'O' || c == 'Q') { valid = false; break; }
            }
            if (valid) {
                Serial.printf("VIN probe: OBD-II VIN %s\n", vinObdBuffer);
                handleVin(vinObdBuffer);
            } else {
                Serial.println("VIN probe: reply did not contain a valid VIN");
            }
        }
    }
}

void manageBmsDiagnostics(unsigned long currentTimeMs) {
#if LEAF_CAN_RX_ONLY
    (void)currentTimeMs;
#else
    extern uint32_t canFramesB;
    extern uint32_t canFramesA;
    extern bool isChargingNow;
    static unsigned long lastActivitySampleMs = 0;
    static uint32_t lastActivityFrames = 0;
    static uint32_t lastActivityFramesA = 0;
    static bool diagnosticWindow = false;
    static unsigned long windowUntilMs = 0;

    if (lastActivitySampleMs == 0 || currentTimeMs - lastActivitySampleMs >= 5000) {
        uint32_t frameDelta = canFramesB - lastActivityFrames;
        uint32_t evDelta = canFramesA - lastActivityFramesA;
        // Back to the .59 behaviour that worked on the car: any bus activity
        // keeps the diagnostic window open.
        bool busActive = isChargingNow || frameDelta >= 20 || evDelta >= 100;
        if (busActive) windowUntilMs = currentTimeMs + 20000;
        diagnosticWindow = busActive || currentTimeMs < windowUntilMs ||
                           currentTimeMs < bmsForceWindowUntilMs;
        lastActivityFrames = canFramesB;
        lastActivityFramesA = canFramesA;
        lastActivitySampleMs = currentTimeMs;
    }
    if (bmsQuery != BmsQuery::NONE) {
        if (currentTimeMs - bmsQueryStartedMs > 3000) {
            // No reply: back this group off and let the others keep their
            // turn. Retrying the same request three times in a row only
            // flooded the gateway and kept cells/temperature out of rotation.
            bmsFailStreak++;
            bmsFailCount++;
            bmsGroupDone(false);
            finishBmsQuery();
        }
        return;
    }
    if (!diagnosticWindow || activeSequence != CanSequence::NONE || vinPending) return;
    if ((long)(currentTimeMs - bmsCooldownUntilMs) < 0) return;
    // Never queue two requests back to back: the gateway needs a breath
    // between ISO-TP transfers.
    if (lastBmsQueryMs != 0 && currentTimeMs - lastBmsQueryMs < 3000) return;

    // First pass after boot: stagger the groups instead of firing all four
    // inside twelve seconds. A burst like that is what made the LBC answer
    // cells and temperatures and then shut the channel.
    for (uint8_t i = 0; i < BMS_GROUP_COUNT; i++) {
        if (bmsGroupDueMs[i] == 0) {
            bmsGroupDueMs[i] = currentTimeMs + (unsigned long)i * 15000UL;
        }
    }

    // Ask the group whose slot is due. Each group keeps its own deadline, so a
    // group that stops answering can no longer starve the rest.
    int8_t pick = -1;
    for (uint8_t i = 0; i < BMS_GROUP_COUNT; i++) {
        if ((long)(currentTimeMs - bmsGroupDueMs[i]) >= 0) {
            if (pick < 0 || (long)(bmsGroupDueMs[i] - bmsGroupDueMs[pick]) < 0) {
                pick = (int8_t)i;
            }
        }
    }
    if (pick < 0) return;
    bmsGroupIndex = (uint8_t)pick;

    uint8_t group = kBmsGroups[bmsGroupIndex];
    const uint8_t request[8] = {0x02, 0x21, group, 0, 0, 0, 0, 0};
    if (!sendCAN(0x79B, request, sizeof(request))) {
        // Rejected by the TX whitelist or dropped by the controller. Back the
        // group off instead of retrying the same blocked group forever.
        Serial.printf("BMS group 0x%02X could not be sent, backing off\n", group);
        bmsFailStreak++;
        bmsFailCount++;
        bmsGroupDone(false);
        lastBmsQueryMs = currentTimeMs;
    } else {
        bmsQuery = kBmsQueryTypes[bmsGroupIndex];
        bmsRequestedGroup = group;
        bmsQueryStartedMs = currentTimeMs;
        lastBmsQueryMs = currentTimeMs;
        bmsRequestCount++;
        Serial.printf("BMS diagnostic group 0x%02X requested on OBD CAN\n", group);
        // Hold the loop on CAN reception until the reply is assembled. Some
        // LBC firmware stops after an internal block and waits for another
        // flow control, so re-send it whenever progress stalls. If nothing at
        // all arrives, give up quickly instead of stalling the loop.
        uint32_t waitStart = millis();
        uint32_t respAtStart = bmsResponseCount;
        uint16_t lastProgress = bmsLastReceivedLength();
        uint32_t lastChange = millis();
        while (bmsQuery != BmsQuery::NONE) {
            uint32_t elapsed = millis() - waitStart;
            bool gotSomething = bmsResponseCount > respAtStart;
            if (elapsed >= (gotSomething ? 1500u : 300u)) break;
            canDrainFor(5);
            uint16_t now = bmsLastReceivedLength();
            if (now != lastProgress) {
                lastProgress = now;
                lastChange = millis();
            } else if (millis() - lastChange > 60) {
                const uint8_t continueFlow[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
                if (sendCAN(0x79B, continueFlow, sizeof(continueFlow))) {
                    lastChange = millis();
                }
            }
        }
    }
#endif
}

uint32_t bmsQueryRequests() { return bmsRequestCount; }
uint32_t bmsQueryResponses() { return bmsResponseCount; }
uint32_t vinQueryRequests() { return vinRequestCount; }
uint32_t vinQueryResponses() { return vinResponseCount; }
uint16_t bmsLastExpectedLength() { return bmsLastExpected; }
uint16_t bmsLastReceivedLength() { return bmsReceivedLength; }
uint8_t bmsLastGroupId() { return bmsLastGroup; }
uint32_t bmsFirstFrames() { return bmsFirstFrameCount; }

uint32_t bmsLastReplyAgeSec() {
    if (bmsLastReplyMs == 0) return 0xFFFFFFFFUL;   // never answered
    return (millis() - bmsLastReplyMs) / 1000UL;
}

uint32_t bmsFailureCount() { return bmsFailCount; }

uint8_t bmsRequestedGroupId() { return bmsRequestedGroup; }

uint32_t bmsCooldownLeftSec() {
    if (bmsCooldownUntilMs == 0) return 0;
    unsigned long now = millis();
    if ((long)(now - bmsCooldownUntilMs) >= 0) return 0;
    return (bmsCooldownUntilMs - now) / 1000UL;
}

// Called right after a wake/command sequence finishes: the gateway answers
// diagnostics for only a few seconds after it is woken (measured: CAR-CAN
// bursts ~5 s and then goes quiet again), so the groups must be asked now
// rather than at the next 5 s activity sample.
void bmsRequestImmediateBurst() {
    unsigned long now = millis();
    for (uint8_t i = 0; i < BMS_GROUP_COUNT; i++) {
        bmsGroupDueMs[i] = now + (unsigned long)i * 2000UL;
        bmsGroupBackoffSec[i] = 0;
    }
    bmsCooldownUntilMs = 0;
    bmsFailStreak = 0;
    bmsForceWindowUntilMs = now + 15000UL;
}

// Sustain the proven climate session with the same requested-temperature byte
// used by its enable/release frames.
static unsigned long lastHvacKeepaliveMs = 0;

void manageHvacSessionKeepalive(unsigned long currentTimeMs, bool sessionActive) {
#if LEAF_CAN_RX_ONLY
    if (!sessionActive) {
        lastHvacKeepaliveMs = 0;
        return;
    }
    // Never interleave with a command sequence; restart the 1 s clock after it.
    if (activeSequence != CanSequence::NONE) {
        lastHvacKeepaliveMs = currentTimeMs;
        return;
    }
    if (lastHvacKeepaliveMs != 0 && currentTimeMs - lastHvacKeepaliveMs < 1000) return;
    uint8_t keepalive[4];
    memcpy(keepalive, idle_data.data, sizeof(keepalive));
    keepalive[2] = internal_hvac_setpoint_byte;
    if (sendCAN(idle_data.identifier, keepalive, sizeof(keepalive))) {
        lastHvacKeepaliveMs = currentTimeMs;
    }
#else
    // OVMS ZE1 sends a bounded 24-frame request and no continuous keepalive.
    (void)currentTimeMs;
    (void)sessionActive;
#endif
}
