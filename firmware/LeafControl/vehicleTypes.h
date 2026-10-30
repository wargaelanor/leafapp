#ifndef VEHICLE_TYPES_H
#define VEHICLE_TYPES_H

// Common vehicle states shared across components
enum class ChargerState { 
    IDLE, 
    CHARGING, 
    FINISHED, 
    INTERRUPTED, 
    WAITING 
};

// HVAC Ventilation modes (from OVMS implementation)
enum class VentilationMode {
    OFF,
    FACE,
    FACE_FEET,
    FEET,
    WINDSCREEN_FEET,
    WINDSCREEN,
    UNKNOWN
};

// EV-CAN battery metrics (passive listener, Port A)
struct EvBatteryData {
    int soh = -1;                 // % , -1 = no data
    int gids = -1;                // raw GIDS, -1 = no data
    float packVoltage = -1.0f;    // V, <0 = no data
    float packCurrent = 0.0f;     // A (discharge positive, charge/regen negative)
    float packPower = 0.0f;       // kW (discharge positive, charge/regen negative)
    bool packDataValid = false;    // instantaneous current/power from 0x1DB
    bool packVoltageValid = false; // voltage from 0x1DB or BMS group 0x02
    float ambientTemp = -99.0f;   // °C, out of range = no data
    int8_t vehicleOn = -1;        // -1 unknown, 0 off, 1 on

    // ZE1 dashboard / BMS derived values
    float instrumentSOC = -1.0f;  // dashboard SOC %, 0x59E byte7 / 2
    int instrumentRange = -1;     // dashboard range (GOM), km, ZE1 0x5A9
    float batteryTemp = -99.0f;   // pack temperature °C, EV-CAN 0x5C0 byte2 / 2 - 40
    int cellDeltaMv = -1;         // max-min cell voltage, mV (-1 = not available)
    uint32_t odometerKm = 0;      // total distance, 0x5C5 bytes 1..3
    char vin[18] = {0};           // ISO-TP 0x797/0x79A service 0x21 0x81
};

#endif
