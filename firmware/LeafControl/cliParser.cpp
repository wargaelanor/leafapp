#include "cliParser.h"
#include <Arduino.h>
#include "configuration.h"
#include "canInterface.h"
#include "evCanLeaf.h"
#include "vehicleTypes.h"

// Live vehicle state, defined in LeafControl.ino (diagnostics only)
extern float currentSOC;
extern float cabinTemp;
extern float currentSetpoint;
extern float currentFanSpeed;
extern bool isChargingNow;
extern bool isHvacOn;
extern bool headlightsState;
extern int8_t lockState;
extern EvBatteryData evData;

static char inputBuffer[128];
static int inputIndex = 0;

void executeCommand(char* cmd) {
    char* command = strtok(cmd, " ");
    if (command == NULL) return;

    if (strcmp(command, "list") == 0) {
        char* option = strtok(NULL, " ");
        bool showSecrets = (option != NULL && strcmp(option, "--secrets") == 0);

        Serial.println("Current Configuration:");
        Serial.printf("  ssid: %s\n", getWifiSSID());
        Serial.printf("  password: %s\n", showSecrets ? getWifiPassword() : "****");
        Serial.printf("  ssid2: %s\n", getWifiSSID2());
        Serial.printf("  password2: %s\n", showSecrets ? getWifiPassword2() : "****");
        Serial.printf("  ssid3: %s\n", getWifiSSID3());
        Serial.printf("  password3: %s\n", showSecrets ? getWifiPassword3() : "****");
        Serial.printf("  ap_ssid: %s\n", getApSSID());
        Serial.printf("  ap_password: %s\n", showSecrets ? getApPassword() : "****");
        Serial.printf("  hostName: %s\n", getHostName());
        Serial.printf("  mqtt_server: %s\n", getMqttServer());
        Serial.printf("  mqtt_port: %d\n", getMqttPort());
        Serial.printf("  mqtt_user: %s\n", getMqttUser());
        Serial.printf("  mqtt_password: %s\n", showSecrets ? getMqttPassword() : "****");
        Serial.printf("  vehicle_id: %s\n", getVehicleId());
        Serial.printf("  mqtt_vehicle_id: %s\n", getMqttVehicleId());
        Serial.printf("  locking_enabled: %s\n", getLockingEnabled() ? "true" : "false");
        Serial.printf("  mqtt_tls: %s\n", getMqttTls() ? "true" : "false");
        Serial.printf("  hass_discovery: %s\n", getHassDiscoveryEnabled() ? "true" : "false");
        Serial.printf("  sleep_enable: %s\n", getSleepEnabled() ? "true" : "false");
        Serial.printf("  sleep_after_h: %d\n", getSleepAfterHours());
         Serial.printf("  wake_fast_min: %d\n", getWakeFastMinutes());
         Serial.printf("  wake_slow_min: %d\n", getWakeSlowMinutes());
         Serial.printf("  modem_enable: %s\n", getModemEnable() ? "true" : "false");
         Serial.printf("  modem_type: %d\n", getModemType());
         Serial.printf("  modem_pwrkey: %d\n", getModemPwrKey());
         Serial.printf("  modem800_apn: %s\n", getModem800Apn());
         Serial.printf("  modem800_user: %s\n", getModem800User());
         Serial.printf("  modem800_pass: %s\n", showSecrets ? getModem800Pass() : "****");
        if (!showSecrets) {
            Serial.println("  (use 'list --secrets' to display secret values)");
        }
    } 
    else if (strcmp(command, "get") == 0) {
        char* key = strtok(NULL, " ");
        if (key != NULL) {
            if (strcmp(key, "ssid") == 0) Serial.println(getWifiSSID());
            else if (strcmp(key, "password") == 0) Serial.println("****");
            else if (strcmp(key, "ssid2") == 0) Serial.println(getWifiSSID2());
            else if (strcmp(key, "password2") == 0) Serial.println("****");
            else if (strcmp(key, "ssid3") == 0) Serial.println(getWifiSSID3());
            else if (strcmp(key, "password3") == 0) Serial.println("****");
            else if (strcmp(key, "ap_ssid") == 0) Serial.println(getApSSID());
            else if (strcmp(key, "ap_password") == 0) Serial.println("****");
            else if (strcmp(key, "hostName") == 0) Serial.println(getHostName());
            else if (strcmp(key, "mqtt_server") == 0) Serial.println(getMqttServer());
            else if (strcmp(key, "mqtt_port") == 0) Serial.println(getMqttPort());
            else if (strcmp(key, "mqtt_user") == 0) Serial.println(getMqttUser());
            else if (strcmp(key, "mqtt_password") == 0) Serial.println("****");
            else if (strcmp(key, "vehicle_id") == 0) Serial.println(getVehicleId());
            else if (strcmp(key, "mqtt_vehicle_id") == 0) Serial.println(getMqttVehicleId());
            else if (strcmp(key, "locking_enabled") == 0) Serial.println(getLockingEnabled() ? "true" : "false");
            else if (strcmp(key, "mqtt_tls") == 0) Serial.println(getMqttTls() ? "true" : "false");
            else if (strcmp(key, "hass_discovery") == 0) Serial.println(getHassDiscoveryEnabled() ? "true" : "false");
            else if (strcmp(key, "sleep_enable") == 0) Serial.println(getSleepEnabled() ? "true" : "false");
            else if (strcmp(key, "sleep_after_h") == 0) Serial.println(getSleepAfterHours());
            else if (strcmp(key, "wake_fast_min") == 0) Serial.println(getWakeFastMinutes());
             else if (strcmp(key, "wake_slow_min") == 0) Serial.println(getWakeSlowMinutes());
             else if (strcmp(key, "modem800_apn") == 0) Serial.println(getModem800Apn());
             else if (strcmp(key, "modem800_user") == 0) Serial.println(getModem800User());
             else if (strcmp(key, "modem800_pass") == 0) Serial.println("****");
            else Serial.println("Unknown parameter.");
        } else {
            Serial.println("Usage: get <key>");
        }
    } 
    else if (strcmp(command, "set") == 0) {
        char* key = strtok(NULL, " ");
        if (key != NULL) {
            char* val = strtok(NULL, ""); // Get rest of the string
            if (val == NULL) val = (char*)""; // Allow empty values

            ConfigStatus status = ConfigStatus::ERROR_UNKNOWN;

            if (strcmp(key, "ssid") == 0) status = setWifiSSID(val);
            else if (strcmp(key, "password") == 0) status = setWifiPassword(val);
            else if (strcmp(key, "ssid2") == 0) status = setWifiSSID2(val);
            else if (strcmp(key, "password2") == 0) status = setWifiPassword2(val);
            else if (strcmp(key, "ssid3") == 0) status = setWifiSSID3(val);
            else if (strcmp(key, "password3") == 0) status = setWifiPassword3(val);
            else if (strcmp(key, "ap_ssid") == 0) status = setApSSID(val);
            else if (strcmp(key, "ap_password") == 0) status = setApPassword(val);
            else if (strcmp(key, "hostName") == 0) status = setHostName(val);
            else if (strcmp(key, "mqtt_server") == 0) status = setMqttServer(val);
            else if (strcmp(key, "mqtt_port") == 0) {
                int port = atoi(val);
                if (port > 0 && port <= 65535) {
                    status = setMqttPort(port);
                } else {
                    status = ConfigStatus::ERROR_PORT_RANGE;
                }
            }
            else if (strcmp(key, "mqtt_user") == 0) status = setMqttUser(val);
            else if (strcmp(key, "mqtt_password") == 0) status = setMqttPassword(val);
            else if (strcmp(key, "vehicle_id") == 0) status = setVehicleId(val);
            else if (strcmp(key, "mqtt_vehicle_id") == 0) status = setMqttVehicleId(val);
            else if (strcmp(key, "locking_enabled") == 0) {
                if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) {
                    status = setLockingEnabled(true);
                } else if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0) {
                    status = setLockingEnabled(false);
                } else {
                    Serial.println("Error: Value must be 'true', 'false', '1', or '0'.");
                    return;
                }
            }
            else if (strcmp(key, "mqtt_tls") == 0) {
                if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) {
                    status = setMqttTls(true);
                } else if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0) {
                    status = setMqttTls(false);
                } else {
                    Serial.println("Error: Value must be 'true', 'false', '1', or '0'.");
                    return;
                }
            }
            else if (strcmp(key, "hass_discovery") == 0) {
                if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) {
                    status = setHassDiscoveryEnabled(true);
                } else if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0) {
                    status = setHassDiscoveryEnabled(false);
                } else {
                    Serial.println("Error: Value must be 'true', 'false', '1', or '0'.");
                    return;
                }
            }
            else if (strcmp(key, "sleep_enable") == 0) {
                if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) {
                    status = setSleepEnabled(true);
                } else if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0) {
                    status = setSleepEnabled(false);
                } else {
                    Serial.println("Error: Value must be 'true', 'false', '1', or '0'.");
                    return;
                }
            }
            else if (strcmp(key, "sleep_after_h") == 0) {
                int v = atoi(val);
                if (v >= 1 && v <= 168) status = setSleepAfterHours(v);
                else { Serial.println("Error: Value must be 1-168 hours."); return; }
            }
            else if (strcmp(key, "wake_fast_min") == 0) {
                int v = atoi(val);
                if (v >= 1 && v <= 1440) status = setWakeFastMinutes(v);
                else { Serial.println("Error: Value must be 1-1440 minutes."); return; }
            }
            else if (strcmp(key, "wake_slow_min") == 0) {
                int v = atoi(val);
                if (v >= 1 && v <= 1440) status = setWakeSlowMinutes(v);
                else { Serial.println("Error: Value must be 1-1440 minutes."); return; }
            }
            else if (strcmp(key, "modem800_apn") == 0) status = setModem800Apn(val);
            else if (strcmp(key, "modem800_user") == 0) status = setModem800User(val);
            else if (strcmp(key, "modem800_pass") == 0) status = setModem800Pass(val);
            else {
                Serial.println("Unknown parameter.");
                return;
            }
            
            if (status == ConfigStatus::OK) {
                Serial.println("Value updated.");
            } else if (status == ConfigStatus::ERROR_LENGTH) {
                Serial.println("Error: Value exceeds maximum allowed length.");
            } else if (status == ConfigStatus::ERROR_PASSWORD_LENGTH) {
                Serial.println("Error: AP Password must be either empty or at least 8 characters long.");
            } else if (status == ConfigStatus::ERROR_HOSTNAME_LENGTH) {
                Serial.println("Error: Hostname must be 1-15 characters long.");
            } else if (status == ConfigStatus::ERROR_HOSTNAME_HYPHEN) {
                Serial.println("Error: Hostname cannot start or end with a hyphen.");
            } else if (status == ConfigStatus::ERROR_HOSTNAME_CHARS) {
                Serial.println("Error: Hostname can only contain letters, numbers, and hyphens.");
            } else if (status == ConfigStatus::ERROR_PORT_RANGE) {
                Serial.println("Error: Invalid port number. Must be 1-65535. Value not updated.");
            } else {
                Serial.println("Error: Failed to update value.");
            }
        } else {
            Serial.println("Usage: set <key> <value>");
        }
    } 
    else if (strcmp(command, "log") == 0) {
        char* option = strtok(NULL, " ");
        if (option != NULL && strcmp(option, "on") == 0) {
            canLogFrames = true;
            Serial.println("CAN frame logging enabled");
        } else if (option != NULL && strcmp(option, "off") == 0) {
            canLogFrames = false;
            Serial.println("CAN frame logging disabled");
        } else {
            Serial.printf("CAN frame logging: %s\n", canLogFrames ? "on" : "off");
            Serial.println("Usage: log on|off");
        }
    }
    else if (strcmp(command, "status") == 0) {
        Serial.println("---- vehicle state ----");
        Serial.printf("SOC: nominal %.1f %% | dashboard %.1f %%\n",
                      currentSOC, evData.instrumentSOC);
        Serial.printf("Range: dashboard %d km\n", evData.instrumentRange);
        Serial.printf("Pack: %.1f V %.1f A (valid %d), battery temp %.1f C\n",
                      evData.packVoltage, evData.packCurrent,
                      evData.packDataValid ? 1 : 0, evData.batteryTemp);
        Serial.printf("SOH %d %% | GIDS %d | cell delta %d mV\n",
                      evData.soh, evData.gids, evData.cellDeltaMv);
        Serial.printf("Ambient %.1f C | cabin %.1f C | setpoint %.1f | fan %.0f\n",
                      evData.ambientTemp, cabinTemp, currentSetpoint, currentFanSpeed);
        Serial.printf("Lock %d | charging %d | hvac %d | lights %d | carOn %d\n",
                      (int)lockState, isChargingNow ? 1 : 0, isHvacOn ? 1 : 0,
                      headlightsState ? 1 : 0, (int)evData.vehicleOn);
        Serial.printf("CAN frames: Port B %lu | Port A %lu\n",
                      (unsigned long)canFramesB, (unsigned long)canFramesA);
        Serial.printf("EV-CAN: %s / %s | lock %d scan %d chip %d cfg %d\n",
                      evCanClockName(), evCanBitrateName(),
                      evCanIsLocked() ? 1 : 0, evCanScanActive() ? 1 : 0,
                      evCanChipCheckOk() ? 1 : 0, evCanConfigWritesOk() ? 1 : 0);
        Serial.printf("EV-CAN errors: REC %u TEC %u EFLG 0x%02X\n",
                      (unsigned)evCanRxErrorCount(), (unsigned)evCanTxErrorCount(),
                      (unsigned)evCanErrorFlags());
    }
    else if (strcmp(command, "evscan") == 0) {
        evCanRequestScan();
        Serial.println("EV-CAN: listen-only clock/bitrate sweep started; run 'status' to see the lock");
    }
    else if (strcmp(command, "reboot") == 0) {
        Serial.println("Rebooting...");
        delay(100);
        ESP.restart();
    } 
    else if (strcmp(command, "factory-reset") == 0) {
        Serial.println("Performing factory reset...");
        factoryReset();
        delay(100);
        ESP.restart();
    }
    else {
        Serial.println("Unknown command.");
    }
}

void processSerialInput() {
    while (Serial.available() > 0) {
        char c = Serial.read();
        
        if (c == '\n' || c == '\r') {
            if (inputIndex > 0) {
                inputBuffer[inputIndex] = '\0';
                executeCommand(inputBuffer);
                inputIndex = 0;
            }
        } 
        else if (inputIndex < (sizeof(inputBuffer) - 1)) {
            inputBuffer[inputIndex++] = c;
        }
    }
}
