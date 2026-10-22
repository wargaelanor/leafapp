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
};

#endif
