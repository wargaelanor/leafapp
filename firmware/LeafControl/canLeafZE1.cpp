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
    // 0x54C (outside ambient) never shows up on Port B. The AC controller
    // publishes its own ambient reading in 0x54A byte7, 0.5 C/bit.
    uint8_t ambRaw = data[7];
    if (ambRaw != 0x00 && ambRaw != 0xFF) {
        float ambient = ambRaw * 0.5f;
        if (ambient >= -40.0f && ambient <= 60.0f) handleEvAmbient(ambient);
    }
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

    handleDoorStatus(fl, fr, rl, rr, trunk);
    handleLockStatus(locked);
    handleHeadlightsStatus(lightsOn);
}

void handleRangeInstrumentMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    // 0x5A9 RangeInstrument (CAR-CAN, 13 bits, Motorola, start bit 15).
    // The ZE1 logs so far only contain "blank" frames (raw 0x1000 / 0x1FFF)
    // and the km scale is not confirmed for this car yet, so the value is kept
    // for diagnostics only — never published as a range.
    uint16_t raw = ((uint16_t)data[1] << 5) | (data[2] >> 3);
    extern uint16_t lastRangeRawA9;
    if (raw != lastRangeRawA9) {
        lastRangeRawA9 = raw;
        Serial.printf("0x5A9 range raw=%u (uncalibrated, not displayed)\n", raw);
    }
    // Clear a stale retained dashboard range once: older firmware published
    // 0x5A9 as km and the PWA would keep showing that value after an update.
    static bool staleRangeCleared = false;
    if (!staleRangeCleared) {
        staleRangeCleared = true;
        handleInstrumentRange(0);
    }
}

uint16_t lastRangeRawA9 = 0;

// 0x5BC — HV battery capacity: GIDS + SOH. Normally an EV-CAN frame, but the
// ZE1 also broadcasts it on IT-CAN (Port B), which is our working bus.
// Verified on car: 3D 80 ... -> GIDS 246 at 61 % dashboard, A1 -> SOH 80 %.
void handleBatteryCapacityMessage(const uint8_t* data, uint8_t len) {
    (void)len;
    static int lastGids = -1;
    static int lastSoh = -1;
    int gids = (data[0] << 2) | (data[1] >> 6);
    int soh  = (data[4] >> 1) & 0x7F;
    if (gids > 0 && gids <= 500 && gids != lastGids) {
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

void handleVinResponse(const uint8_t* data, uint8_t len) {
    if (!vinPending || len < 2) return;
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
            handleVin(vinBuffer);
        }
    }
}

// Message parser dispatch table
static const MessageParser parsers[] = {
    CAN_PARSER(raw_soc_readout, handleSOCMessage),
    CAN_PARSER(cabin_temp_readout, handleTempMessage),
    CAN_PARSER(car_awake_readout, handleCarAwakeMessage),
    CAN_PARSER(charger_status_readout, handleChargerMessage),
    CAN_PARSER(hvac_status_readout, handleHVACMessage),
    CAN_PARSER(hvac_setpoint_readout, handleHVACSetpointMessage),
    CAN_PARSER(doors_and_locks_readout, handleDoorsAndLocksMessage),
    CAN_PARSER(range_instrument_readout, handleRangeInstrumentMessage),
    CAN_PARSER(battery_capacity_readout, handleBatteryCapacityMessage),
    CAN_PARSER(battery_temp_hvb_readout, handleBatteryTempHvbMessage),
    CAN_PARSER(battery_temp_vcm_readout, handleBatteryTempVcmMessage),
    CAN_PARSER(odometer_units_readout, handleOdometerUnitsMessage),
    CAN_PARSER(odometer_readout, handleOdometerMessage),
    { 0x79A, 1, handleVinResponse },
};

// Can message reception handler, to be called when a CAN message is received
void handleReceivedMessage(uint32_t identifier, uint8_t* data, uint8_t len) {
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
    // The remote-climate frames carry a fixed OEM flag in byte 2 (0x0A) and no
    // setpoint at all — the BCM keeps the setpoint that is configured in the
    // car. The requested value is only recorded for diagnostics/UI.
    internal_hvac_setpoint_byte = (setpoint >= 16.0f && setpoint <= 32.0f)
        ? static_cast<uint8_t>(setpoint * 2.0f)
        : 0;
    Serial.printf("HVAC setpoint request: %.1f C (byte %u kept at OEM 0x0A)\n",
                  setpoint, internal_hvac_setpoint_byte);
}

static const SequenceStep seq_refresh[] = {
    MSG_BURST_STEP(hvac_init, 6, 100, 3, 800),
    MSG_STEP(idle_data, 4, 100)
};

// Remote climate ON, payloads and cadence from "запуск и стоп климата через тсу":
// 46 08 00 00 x7 -> 4E 08 0A 00 x7 -> 8E 00 0A 00 x8 -> 86 00 0A 00 (kept
// running by manageHvacSessionKeepalive() while the session is active).
static const SequenceStep seq_hvac_on[] = {
    MSG_BURST_STEP(hvac_init, 7, 100, 3, 800),
    MSG_BURST_STEP(hvac_on_data, 7, 100, 3, 800),
    MSG_BURST_STEP(hvac_on_idle, 8, 100, 3, 800),
    MSG_BURST_STEP(hvac_session_idle, 12, 100, 3, 800)
};

// Remote climate OFF: 0x54B flips to "off" on the first 56 08 0A frame, the
// rest is the OEM release tail.
static const SequenceStep seq_hvac_off[] = {
    MSG_STEP(hvac_init, 1, 100),
    MSG_BURST_STEP(hvac_off_data, 3, 100, 3, 800),
    MSG_BURST_STEP(interrupt_data, 3, 100, 3, 800),
    MSG_BURST_STEP(hvac_off_data, 3, 100, 3, 800),
    MSG_BURST_STEP(interrupt_data, 2, 100, 3, 800),
    MSG_STEP(idle_data, 3, 100)
};

static const SequenceStep seq_charge_on[] = {
    MSG_STEP(start_charge_data, 37, 110),   // OEM: 66 08 00 00 x37 over ~4 s
    MSG_STEP(charge_ack_data, 1, 100),      // OEM: single A6 00 00 00
    MSG_STEP(idle_data, 3, 100)
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

// Unlock: OEM sets bit0 of byte0 (46 -> 47) and repeats the request about
// 0.8 s later. Only a couple of frames — long bursts made the locks click.
static const SequenceStep seq_unlock_doors[] = {
    MSG_STEP(tcu_standby, 1, 100),
    MSG_STEP(unlock_doors_data, 2, 100),
    MSG_WAIT_STEP(800),
    MSG_STEP(unlock_doors_data, 2, 100),
    MSG_STEP(tcu_standby, 2, 100)
};

// Lock: OEM sends exactly two 46 80 frames 100 ms apart, then standby.
// The old 24x "60 80" burst was seen by the BCM as repeated lock events and
// made the actuators click 8-10 times without latching.
static const SequenceStep seq_lock_doors[] = {
    MSG_STEP(tcu_standby, 1, 100),
    MSG_STEP(lock_doors_data, 2, 100),
    MSG_STEP(tcu_standby, 2, 100)
};

static const SequenceStep seq_lights[] = {
    MSG_BURST_STEP(hvac_init, 6, 100, 3, 800),
    MSG_STEP(lights_data, 20, 100),
    MSG_STEP(hvac_init, 4, 100),
    MSG_STEP(idle_data, 3, 100)
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
    if (seq == CanSequence::NONE) return false;
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
            if (carIsAwake || wakeAttempts >= 20) {
                if (!carIsAwake) {
                    Serial.println("### Wake: no reply, sending command anyway ###");
                }
                currentPhase = SeqPhase::WAKE_WAIT;
                nextCANEventTime = currentTimeMs + (carIsAwake ? 50 : 100);
                return carIsAwake ? CanSeqResult::WAKE_SUCCESS : CanSeqResult::WAKE_TIMEOUT;
            } else {
                sendCAN(wakeup_data);
                wakeAttempts++;
                nextCANEventTime = currentTimeMs + 15;
            }
            break;
            
        case SeqPhase::WAKE_WAIT:
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
    extern uint32_t canFramesB;
    if (vinBuffer[0] != '\0' || canFramesB == 0 || activeSequence != CanSequence::NONE) return;
    if (vinPending && currentTimeMs - lastVinRequestMs < 5000) return;
    if (lastVinRequestMs != 0 && currentTimeMs - lastVinRequestMs < 30000) return;
    const uint8_t request[8] = {0x02, 0x21, 0x81, 0, 0, 0, 0, 0};
    if (sendCAN(0x797, request, sizeof(request))) {
        vinPending = true;
        lastVinRequestMs = currentTimeMs;
        Serial.println("VIN request sent on IT-CAN");
    }
}

// The OEM TCU sustains a remote climate session by repeating "86 00 0A 00"
// roughly once per second. Without that heartbeat the BCM drops the request
// within seconds, which looked like "climate does not start".
static unsigned long lastHvacKeepaliveMs = 0;

void manageHvacSessionKeepalive(unsigned long currentTimeMs, bool sessionActive) {
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
    if (sendCAN(hvac_session_idle)) lastHvacKeepaliveMs = currentTimeMs;
}
