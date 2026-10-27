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
    float packCurrent = 0.0f;     // A (signed, charge positive)
    bool packDataValid = false;
    float ambientTemp = -99.0f;   // °C, out of range = no data
    int8_t vehicleOn = -1;        // -1 unknown, 0 off, 1 on

    // ZE1 dashboard / BMS derived values
    float instrumentSOC = -1.0f;  // dashboard SOC %, 0x59E byte7 / 2
    int instrumentRange = -1;     // dashboard range (GOM), km, 0x5A9 / 16
    float batteryTemp = -99.0f;   // pack temperature °C, 0x5B3 / 0x5C0 byte0 * 0.25
    int cellDeltaMv = -1;         // max-min cell voltage, mV (-1 = not available)
};

#endif
