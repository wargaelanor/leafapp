#ifndef CAN_LEAF_ZE1_H
#define CAN_LEAF_ZE1_H

#include "canInterface.h"
#include "vehicleTypes.h"

// Leaf readout messages
inline constexpr CANMessage<2> raw_soc_readout = {0x55B};
inline constexpr CANMessage<1> cabin_temp_readout = {0x54F};
inline constexpr CANMessage<0> car_awake_readout = {0x601};
inline constexpr CANMessage<6> charger_status_readout = {0x390};
inline constexpr CANMessage<2> hvac_status_readout = {0x54B};
inline constexpr CANMessage<5> hvac_setpoint_readout = {0x54A};
inline constexpr CANMessage<3> doors_and_locks_readout = {0x60D};
inline constexpr CANMessage<2> range_instrument_readout = {0x5A9};
inline constexpr CANMessage<7> battery_capacity_readout = {0x5BC}; // GIDS + SOH (also present on IT-CAN)
inline constexpr CANMessage<8> battery_temp_hvb_readout = {0x5C0}; // LBC pack temperature (mux 1/2/3)
inline constexpr CANMessage<5> battery_temp_vcm_readout = {0x5B3}; // VCM pack temperature (fallback)
inline constexpr CANMessage<4> odometer_readout = {0x5C5};
inline constexpr CANMessage<7> odometer_units_readout = {0x355};

// Leaf can messages.
// Payloads below are copied from the OEM TCU (NissanConnect) captures, see
// "закрыть через тсу.txt" / "запуск и стоп климата через тсу.txt".
// 0x56E byte0 = command, byte1 = sub-command, byte2 = climate/charge flag.
inline constexpr CANMessage<1> wakeup_data       = {0x68C, {0x00}};                  // Wakeup ping
inline constexpr CANMessage<4> hvac_init         = {0x56E, {0x46, 0x08, 0x00, 0x00}}; // TCU idle / keepalive
inline constexpr CANMessage<4> tcu_standby       = {0x56E, {0x46, 0x00, 0x00, 0x00}}; // TCU standby (between commands)
inline constexpr CANMessage<4> idle_data         = {0x56E, {0x86, 0x00, 0x00, 0x00}}; // Go to sleep
inline constexpr CANMessage<4> hvac_on_data      = {0x56E, {0x4E, 0x08, 0x0A, 0x00}}; // Remote climate ON (OEM)
inline constexpr CANMessage<4> hvac_on_idle      = {0x56E, {0x8E, 0x00, 0x0A, 0x00}}; // Climate ON acknowledgement phase
inline constexpr CANMessage<4> hvac_session_idle = {0x56E, {0x86, 0x00, 0x0A, 0x00}}; // Session keepalive (OEM repeats ~1/s)
inline constexpr CANMessage<4> start_charge_data = {0x56E, {0x66, 0x08, 0x00, 0x00}}; // Charging ON (OEM)
inline constexpr CANMessage<4> charge_ack_data   = {0x56E, {0xA6, 0x00, 0x00, 0x00}}; // Charging ON final ack (OEM)
inline constexpr CANMessage<4> interrupt_data    = {0x56E, {0x96, 0x00, 0x00, 0x00}}; // Interrupt action (generic)
inline constexpr CANMessage<4> hvac_off_int      = {0x56E, {0x96, 0x00, 0x0A, 0x00}}; // OEM climate OFF interrupt
inline constexpr CANMessage<4> hvac_off_data     = {0x56E, {0x56, 0x08, 0x0A, 0x00}}; // HVAC OFF (OEM)
// Lock: OEM sends exactly two 46 80 frames 100 ms apart, then standby.
// Unlock: OEM sets bit0 of byte0 (46 -> 47), also repeated twice.
inline constexpr CANMessage<4> unlock_doors_data = {0x56E, {0x47, 0x00, 0x00, 0x00}}; // Unlock doors (OEM)
inline constexpr CANMessage<4> lock_doors_data   = {0x56E, {0x46, 0x80, 0x00, 0x00}}; // Lock doors (OEM)
inline constexpr CANMessage<4> lights_data       = {0x56E, {0x46, 0x09, 0x12, 0x10}}; // Flash lights (RHL=1, captured)
inline constexpr CANMessage<4> horn_light_data   = {0x56E, {0x46, 0x0B, 0x12, 0x10}}; // Flash + horn (RHL=3, captured)

// ===== CAN Field Definitions (bit 0 = MSB of byte 0) =====
inline constexpr CANField soc_field = { 0, 10 };    
inline constexpr CANField cabin_temp_field = { 0, 8 }; 
inline constexpr CANField cStatus_field   = { 41, 6 }; 
inline constexpr CANField qc_state_field  = { 33, 1 }; 
inline constexpr CANField vg_state_field   = { 0, 4 };  
inline constexpr CANField chargeVolt_field = { 27, 2 }; 
inline constexpr CANField hvac_bit0_field  = { 15, 1 };
inline constexpr CANField hvac_bit4_field  = { 11, 1 }; 
inline constexpr CANField hvac_bit5_field  = { 10, 1 }; 
inline constexpr CANField hvac_setpoint_field = { 32, 8 };
inline constexpr CANField fan_speed_field = { 34, 3 }; // Bits 5, 4, 3 (MSB bit 7 is 32)
inline constexpr CANField ventilation_mode_field = { 16, 8 }; // Byte 2 contains ventilation mode
inline constexpr CANField door_trunk_field = { 0, 1 };
inline constexpr CANField door_rl_field    = { 1, 1 };
inline constexpr CANField door_rr_field    = { 2, 1 };
inline constexpr CANField door_fr_field    = { 3, 1 };
inline constexpr CANField door_fl_field    = { 4, 1 };
inline constexpr CANField locked_field     = { 19, 1 };
inline constexpr CANField headlight_dip_field  = { 6, 1 };   // byte0 & 0x02 (dip beam)
inline constexpr CANField headlight_main_field = { 12, 1 };  // byte1 & 0x08 (main beam)

// Scaling definitions
inline constexpr FieldScaling soc_scaling        = {false, 0.1f,   0.0f};
inline constexpr FieldScaling cabin_temp_scaling = {false, 0.5f,  -40.0f};
inline constexpr FieldScaling voltage_scaling    = {false, 110.0f, 0.0f};
inline constexpr FieldScaling setpoint_scaling   = {false, 0.5f,   0.0f};
inline constexpr FieldScaling fan_speed_scaling  = {false, 100.0f/7, 0.0f};

// Readout callbacks (to be called when a message is received, implemented at application side):
void handleRawSOC(float soc);
void handleCabinTemp(float temp);
void handleCarAwake();
void handleChargerStatus(bool isCharging, ChargerState state);
void handleHVACStatus(bool isOn);
void handleHVACSetpoint(float setpoint);
void handleFanSpeed(float speed);
void handleHeatingMode(bool heating, bool cooling);
void handleVentilationMode(VentilationMode mode);
void handleDoorStatus(bool fl, bool fr, bool rl, bool rr, bool trunk);
void handleLockStatus(bool locked);
void handleHeadlightsStatus(bool on);
void handleInstrumentRange(int km);
void handleBatteryTemp(float temp);
void handleOdometer(uint32_t km);
void handleVin(const char* vin);


// Sequence State Machine
enum class CanSequence {
    NONE,
    REFRESH,
    HVAC_ON,
    HVAC_OFF,
    CHARGE_ON,
    CHARGE_OFF,
    UNLOCK_DOORS,
    LOCK_DOORS,
    LIGHTS,
    HORN_LIGHTS,
    RHL_OFF
};

enum class CanSeqResult { IDLE, PROCESSING, WAKE_SUCCESS, WAKE_TIMEOUT, SEQUENCE_FINISHED };

extern CanSequence activeSequence;

// Sequence Payload Modifier Callback Type
typedef void (*SequencePayloadModifier)(uint8_t* data, uint8_t len);

// Application layer payload modifiers
void setHVACTargetTemperature(float setpoint);

// Returns false if another sequence is still active (caller should report "busy")
bool startSequence(CanSequence seq, unsigned long currentTimeMs);
CanSeqResult manageCANSequence(unsigned long currentTimeMs);
void manageVinQuery(unsigned long currentTimeMs);

// The OEM TCU keeps sending 0x56E "86 00 0A 00" about once per second while a
// remote climate session is running. Calling this from loop() while a remote
// climate session is active keeps that heartbeat going (no-op when a command
// sequence is in progress).
void manageHvacSessionKeepalive(unsigned long currentTimeMs, bool sessionActive);

#endif
