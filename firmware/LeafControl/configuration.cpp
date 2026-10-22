#include "configuration.h"
#include <Preferences.h>
#include "stringBuffer.h"

static Preferences preferences;
static bool nvsReady = false;

template <size_t N>
class ConfigString {
private:
    StringBuffer<N> value;
    const char* key;
    const char* default_val;
public:
    ConfigString(const char* k, const char* def) : key(k), default_val(def) {}
    
    void load() {
        if (nvsReady && preferences.isKey(key)) {
            preferences.getString(key, value.data(), value.capacity());
        } else {
            value.copyFrom(default_val);
            if (nvsReady) preferences.putString(key, value.c_str());
        }
    }
    
    ConfigStatus set(const char* val) {
        if (strlen(val) >= value.capacity()) {
            return ConfigStatus::ERROR_LENGTH;
        }
        value.copyFrom(val);
        if (nvsReady) preferences.putString(key, value.c_str());
        return ConfigStatus::OK;
    }
    
    const char* get() const { 
        return value.c_str(); 
    }
};

class ConfigInt {
private:
    int value;
    const char* key;
    int default_val;
public:
    ConfigInt(const char* k, int def) : key(k), default_val(def) {}
    
    void load() {
        if (nvsReady && preferences.isKey(key)) {
            value = preferences.getInt(key, default_val);
        } else {
            value = default_val;
            if (nvsReady) preferences.putInt(key, value);
        }
    }
    
    ConfigStatus set(int val) {
        value = val;
        if (nvsReady) preferences.putInt(key, value);
        return ConfigStatus::OK;
    }
    
    int get() const { 
        return value; 
    }
};

class ConfigBool {
private:
    bool value;
    const char* key;
    bool default_val;
public:
    ConfigBool(const char* k, bool def) : value(def), key(k), default_val(def) {}
    
    void load() {
        if (nvsReady && preferences.isKey(key)) {
            value = preferences.getBool(key, default_val);
        } else {
            value = default_val;
            if (nvsReady) preferences.putBool(key, value);
        }
    }
    
    ConfigStatus set(bool val) {
        value = val;
        if (nvsReady) preferences.putBool(key, value);
        return ConfigStatus::OK;
    }
    
    bool get() const { 
        return value; 
    }
};

// Define config objects
static ConfigString<64> conf_ssid("ssid", "");
static ConfigString<64> conf_password("password", "");
static ConfigString<64> conf_ap_ssid("ap_ssid", "");
static ConfigString<64> conf_ap_password("ap_password", "");
static ConfigString<64> conf_hostName("hostName", "leaf");
static ConfigString<64> conf_mqtt_server("mqtt_server", "");
static ConfigInt        conf_mqtt_port("mqtt_port", 8883);
static ConfigString<64> conf_mqtt_user("mqtt_user", "");
static ConfigString<64> conf_mqtt_password("mqtt_password", "");
static ConfigString<64> conf_vehicle_id("vehicle_id", "");
static ConfigBool       conf_locking_enabled("locking_enabled", false);
static ConfigBool       conf_mqtt_tls("mqtt_tls", true);
static ConfigBool       conf_hass_discovery("hass_discovery", true);
static ConfigBool       conf_sleep_enabled("sleep_enable", false);
static ConfigInt        conf_sleep_after_h("sleep_after_h", 24);
static ConfigInt        conf_wake_fast_min("wake_fast_min", 10);
static ConfigInt        conf_wake_slow_min("wake_slow_min", 60);
static ConfigBool       conf_modem_enable("modem_enable", false);
static ConfigInt        conf_modem_type("modem_type", 0); // 0=SIM800, 1=A7670
static ConfigInt        conf_modem_pwrkey("modem_pwrkey", 0); // GPIO, 0=not managed
static ConfigString<64> conf_modem800_apn("modem800_apn", "");
static ConfigString<64> conf_modem800_user("modem800_user", "");
static ConfigString<64> conf_modem800_pass("modem800_pass", "");
static ConfigString<64> conf_modem7670_apn("modem7670_apn", "");
static ConfigString<64> conf_modem7670_user("modem7670_user", "");
static ConfigString<64> conf_modem7670_pass("modem7670_pass", "");
static ConfigBool       conf_modem7670_gps("modem7670_gps", true);

void initConfiguration() {
    nvsReady = preferences.begin("config", false);
    if (!nvsReady) {
        Serial.println("Error: Failed to initialize NVS (Preferences). Falling back to RAM defaults.");
    }

    conf_ssid.load();
    conf_password.load();
    conf_ap_ssid.load();
    conf_ap_password.load();
    conf_hostName.load();
    conf_mqtt_server.load();
    conf_mqtt_port.load();
    conf_mqtt_user.load();
    conf_mqtt_password.load();
    conf_vehicle_id.load();
    conf_locking_enabled.load();
    conf_mqtt_tls.load();
    conf_hass_discovery.load();
    conf_sleep_enabled.load();
    conf_sleep_after_h.load();
    conf_wake_fast_min.load();
    conf_wake_slow_min.load();
    conf_modem_enable.load();
    conf_modem_type.load();
    conf_modem_pwrkey.load();
    conf_modem800_apn.load();
    conf_modem800_user.load();
    conf_modem800_pass.load();
    conf_modem7670_apn.load();
    conf_modem7670_user.load();
    conf_modem7670_pass.load();
    conf_modem7670_gps.load();
}

void factoryReset() {
    if (!nvsReady) {
        Serial.println("Error: Factory reset failed because NVS is not initialized.");
        return;
    }

    if (preferences.clear()) {
        Serial.println("Factory reset completed successfully.");
    } else {
        Serial.println("Error: Factory reset failed while clearing NVS.");
    }
}

const char* getWifiSSID() { return conf_ssid.get(); }
ConfigStatus setWifiSSID(const char* value) { return conf_ssid.set(value); }

const char* getWifiPassword() { return conf_password.get(); }
ConfigStatus setWifiPassword(const char* value) { return conf_password.set(value); }

const char* getApSSID() { return conf_ap_ssid.get(); }
ConfigStatus setApSSID(const char* value) { return conf_ap_ssid.set(value); }

const char* getApPassword() { return conf_ap_password.get(); }
ConfigStatus setApPassword(const char* value) { 
    size_t len = strlen(value);
    if (len > 0 && len < 8) {
        return ConfigStatus::ERROR_PASSWORD_LENGTH;
    }
    return conf_ap_password.set(value); 
}

const char* getHostName() { return conf_hostName.get(); }
ConfigStatus setHostName(const char* value) {
    size_t len = strlen(value);
    if (len < 1 || len > 15) {
        return ConfigStatus::ERROR_HOSTNAME_LENGTH;
    }
    if (value[0] == '-' || value[len - 1] == '-') {
        return ConfigStatus::ERROR_HOSTNAME_HYPHEN;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isalnum(value[i]) && value[i] != '-') {
            return ConfigStatus::ERROR_HOSTNAME_CHARS;
        }
    }
    return conf_hostName.set(value);
}

const char* getMqttServer() { return conf_mqtt_server.get(); }
ConfigStatus setMqttServer(const char* value) { return conf_mqtt_server.set(value); }

int getMqttPort() { return conf_mqtt_port.get(); }
ConfigStatus setMqttPort(int value) { return conf_mqtt_port.set(value); }

const char* getMqttUser() { return conf_mqtt_user.get(); }
ConfigStatus setMqttUser(const char* value) { return conf_mqtt_user.set(value); }

const char* getMqttPassword() { return conf_mqtt_password.get(); }
ConfigStatus setMqttPassword(const char* value) { return conf_mqtt_password.set(value); }

const char* getVehicleId() { return conf_vehicle_id.get(); }
ConfigStatus setVehicleId(const char* value) { return conf_vehicle_id.set(value); }

bool getLockingEnabled() { return conf_locking_enabled.get(); }
ConfigStatus setLockingEnabled(bool value) { return conf_locking_enabled.set(value); }

bool getMqttTls() { return conf_mqtt_tls.get(); }
ConfigStatus setMqttTls(bool value) { return conf_mqtt_tls.set(value); }

bool getHassDiscoveryEnabled() { return conf_hass_discovery.get(); }
ConfigStatus setHassDiscoveryEnabled(bool value) { return conf_hass_discovery.set(value); }

bool getSleepEnabled() { return conf_sleep_enabled.get(); }
ConfigStatus setSleepEnabled(bool value) { return conf_sleep_enabled.set(value); }

int getSleepAfterHours() {
    int v = conf_sleep_after_h.get();
    return (v < 1) ? 24 : v;
}
ConfigStatus setSleepAfterHours(int value) {
    if (value < 1 || value > 168) return ConfigStatus::ERROR_PORT_RANGE;
    return conf_sleep_after_h.set(value);
}

int getWakeFastMinutes() {
    int v = conf_wake_fast_min.get();
    return (v < 1) ? 10 : v;
}
ConfigStatus setWakeFastMinutes(int value) {
    if (value < 1 || value > 1440) return ConfigStatus::ERROR_PORT_RANGE;
    return conf_wake_fast_min.set(value);
}

int getWakeSlowMinutes() {
    int v = conf_wake_slow_min.get();
    return (v < 1) ? 60 : v;
}
ConfigStatus setWakeSlowMinutes(int value) {
    if (value < 1 || value > 1440) return ConfigStatus::ERROR_PORT_RANGE;
    return conf_wake_slow_min.set(value);
}

bool getModemEnable() { return conf_modem_enable.get(); }
ConfigStatus setModemEnable(bool value) { return conf_modem_enable.set(value); }

int getModemType() { return conf_modem_type.get(); }
ConfigStatus setModemType(int value) {
    if (value < 0 || value > 1) return ConfigStatus::ERROR_PORT_RANGE;
    return conf_modem_type.set(value);
}

int getModemPwrKey() { return conf_modem_pwrkey.get(); }
ConfigStatus setModemPwrKey(int value) {
    if (value < 0 || value > 48) return ConfigStatus::ERROR_PORT_RANGE;
    return conf_modem_pwrkey.set(value);
}

const char* getModem800Apn() { return conf_modem800_apn.get(); }
ConfigStatus setModem800Apn(const char* value) { return conf_modem800_apn.set(value); }

const char* getModem800User() { return conf_modem800_user.get(); }
ConfigStatus setModem800User(const char* value) { return conf_modem800_user.set(value); }

const char* getModem800Pass() { return conf_modem800_pass.get(); }
ConfigStatus setModem800Pass(const char* value) { return conf_modem800_pass.set(value); }

const char* getModem7670Apn() { return conf_modem7670_apn.get(); }
ConfigStatus setModem7670Apn(const char* value) { return conf_modem7670_apn.set(value); }

const char* getModem7670User() { return conf_modem7670_user.get(); }
ConfigStatus setModem7670User(const char* value) { return conf_modem7670_user.set(value); }

const char* getModem7670Pass() { return conf_modem7670_pass.get(); }
ConfigStatus setModem7670Pass(const char* value) { return conf_modem7670_pass.set(value); }

bool getModem7670Gps() { return conf_modem7670_gps.get(); }
ConfigStatus setModem7670Gps(bool value) { return conf_modem7670_gps.set(value); }
