#include "cliParser.h"
#include <Arduino.h>
#include "configuration.h"
#include "canInterface.h"
#include "evCanLeaf.h"
#include "vehicleTypes.h"
#include "udsLeaf.h"

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
        Serial.printf("  locking_enabled: %s\n", getLockingEnabled() ? "true" : "false");
        Serial.printf("  sleep_enable: %s\n", getSleepEnabled() ? "true" : "false");
        Serial.printf("  uds_enable: %s\n", getUdsEnabled() ? "true" : "false");
        Serial.printf("  uds_tpms: %s\n", getUdsTpmsEnabled() ? "true" : "false");
        Serial.printf("  sleep_after_h: %d\n", getSleepAfterHours());
         Serial.printf("  wake_fast_min: %d\n", getWakeFastMinutes());
         Serial.printf("  wake_slow_min: %d\n", getWakeSlowMinutes());
         Serial.printf("  charge_limit_pct: %d\n", getChargeLimitPct());
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
        else if (strcmp(key, "remote_mode") == 0) Serial.println((int)getRemoteTransport());
            else if (strcmp(key, "locking_enabled") == 0) Serial.println(getLockingEnabled() ? "true" : "false");
            else if (strcmp(key, "sleep_enable") == 0) Serial.println(getSleepEnabled() ? "true" : "false");
            else if (strcmp(key, "uds_enable") == 0) Serial.println(getUdsEnabled() ? "true" : "false");
            else if (strcmp(key, "uds_tpms") == 0) Serial.println(getUdsTpmsEnabled() ? "true" : "false");
            else if (strcmp(key, "sleep_after_h") == 0) Serial.println(getSleepAfterHours());
            else if (strcmp(key, "wake_fast_min") == 0) Serial.println(getWakeFastMinutes());
            else if (strcmp(key, "charge_limit_pct") == 0) Serial.println(getChargeLimitPct());
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

            if (strcmp(key, "remote_mode") == 0) {
              int m = atoi(val);
              status = setRemoteTransport(m == 1 ? RemoteTransport::SERVER : RemoteTransport::LOCAL);
              setUiLite(m == 1);
            }
            else if (strcmp(key, "ssid") == 0) status = setWifiSSID(val);
            else if (strcmp(key, "password") == 0) status = setWifiPassword(val);
            else if (strcmp(key, "ssid2") == 0) status = setWifiSSID2(val);
            else if (strcmp(key, "password2") == 0) status = setWifiPassword2(val);
            else if (strcmp(key, "ssid3") == 0) status = setWifiSSID3(val);
            else if (strcmp(key, "password3") == 0) status = setWifiPassword3(val);
            else if (strcmp(key, "ap_ssid") == 0) status = setApSSID(val);
            else if (strcmp(key, "ap_password") == 0) status = setApPassword(val);
            else if (strcmp(key, "hostName") == 0) status = setHostName(val);
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
            else if (strcmp(key, "uds_enable") == 0 || strcmp(key, "uds_tpms") == 0) {
                bool on = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
                bool off = (strcmp(val, "false") == 0 || strcmp(val, "0") == 0);
                if (!on && !off) {
                    Serial.println("Error: Value must be 'true', 'false', '1', or '0'.");
                    return;
                }
                status = (strcmp(key, "uds_enable") == 0) ? setUdsEnabled(on)
                                                          : setUdsTpmsEnabled(on);
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
            else if (strcmp(key, "charge_limit_pct") == 0) {
                int v = atoi(val);
                if (v == 0 || (v >= 70 && v <= 95)) status = setChargeLimitPct(v);
                else { Serial.println("Error: Value must be 0 or 70-95."); return; }
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
    else if (strcmp(command, "uds") == 0) {
        Serial.println("---- Tier-1 UDS (Leaf2018-CAN) ----");
        Serial.printf("enabled %d | tpms %d | in-flight %d\n",
                      getUdsEnabled() ? 1 : 0, getUdsTpmsEnabled() ? 1 : 0, udsQueryBusy() ? 1 : 0);
        Serial.printf("gear '%s' | 12V %.2f V %.2f A | plug %d | mode %d | speed %.1f km/h\n",
                      udsGearText(), udsData.v12, udsData.i12,
                      udsData.plug, udsData.chargeMode, udsData.speed);
        Serial.printf("OBC %d W | QC #%lu | L1L2 #%lu | motor %.0f C | swa %.0f deg\n",
                      udsData.obcW, (unsigned long)udsData.qcCount,
                      (unsigned long)udsData.l1l2Count, udsData.motorTempC, udsData.swaDeg);
        Serial.printf("brake %d | accel %d | eco %d | e-pedal %d | ac %d | rear-htr %d\n",
                      udsData.brakeRaw, udsData.accelPos, udsData.eco, udsData.epedal,
                      udsData.acOn, udsData.rearHeater);
        Serial.printf("TPMS kPa: FR %.0f FL %.0f RR %.0f RL %.0f (mask 0x%02X)\n",
                      udsData.tpmsKpa[0], udsData.tpmsKpa[1],
                      udsData.tpmsKpa[2], udsData.tpmsKpa[3], udsData.tpmsSeen);
        Serial.printf("req %lu | resp %lu\n",
                      (unsigned long)udsData.reqCount, (unsigned long)udsData.respCount);
        Serial.printf("sessions live mask 0x%02X (VCM,BCM,ABS,TMI) | refused 0x%02X\n",
                      udsSessionMask(), udsSessionRefusedMask());
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
