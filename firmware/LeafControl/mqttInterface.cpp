#include "mqttInterface.h"
#include <WiFi.h>
#include <esp_system.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include "configuration.h"
#include "stringBuffer.h"
#include "hassDiscovery.h"
#include "modemManager.h"

WiFiClient espClient; // For non-TLS
WiFiClientSecure espClientSecure; // For TLS
PubSubClient mqttClient;
unsigned long lastMqttReconnectAttempt = 0;

struct OvmsCommands {
  const char* hvacOn = "climatecontrol on";
  const char* hvacOff = "climatecontrol off";
  const char* chargeOn = "charge start";
  const char* chargeOff = "charge stop";
  const char* status = "server v3 update modified";
  const char* lock = "lock";
  const char* unlock = "unlock";
  const char* lights = "lights";
  const char* rhlOff = "lights off";      // must be checked before "lights"
  const char* hornlights = "hornlights";
};
static const OvmsCommands ovmsCmds;

static float lastSOC = -1.0;
static bool socDirty = false;
static float lastCabinTemp = -99.0;
static bool cabinTempDirty = false;
static bool lastChargingStateSet = false;
static bool lastIsCharging = false;
static ChargerState lastChargerState = ChargerState::IDLE;
static bool chargingDirty = false;
static bool lastHvacStateSet = false;
static bool lastIsHvacOn = false;
static bool hvacDirty = false;
static bool lastDoorsStateSet = false;
static bool lastDoorFL = false;
static bool lastDoorFR = false;
static bool lastDoorRL = false;
static bool lastDoorRR = false;
static bool lastDoorTrunk = false;
static bool doorsDirty = false;
static bool lastLockStateSet = false;
static bool lastLockState = false;
static bool lockDirty = false;

static float lastSetpoint = -1.0;
static bool setpointDirty = false;
static float lastFanSpeed = -1.0;
static bool fanSpeedDirty = false;
static bool lastHeating = false;
static bool heatingDirty = false;
static bool lastCooling = false;
static bool coolingDirty = false;
static VentilationMode lastVentilationMode = VentilationMode::UNKNOWN;
static bool ventilationModeDirty = false;

static int lastSOH = -1;
static bool sohDirty = false;
static int lastGids = -1;
static bool gidsDirty = false;
static float lastPackVoltage = -1.0f;
static float lastPackCurrent = 0.0f;
static bool packDirty = false;
static float lastAmbient = -99.0f;
static bool ambientDirty = false;
static bool lastVehicleOnSet = false;
static bool lastVehicleOn = false;
static bool vehicleOnDirty = false;
static bool lastHeadlightsSet = false;
static bool lastHeadlights = false;
static bool headlightsDirty = false;
static bool lastGpsSet = false;
static float lastGpsLat = 0, lastGpsLon = 0, lastGpsAlt = 0, lastGpsSpd = 0;
static bool gpsDirty = false;
static uint32_t lastOdometer = 0;
static String lastVin;
static uint16_t lastHvacTimer = 0;
static float lastInstrumentSoc = -1.0f;
static bool instrumentSocDirty = false;
static int lastInstrumentRange = -1;
static bool instrumentRangeDirty = false;
static float lastBatteryTemp = -99.0f;
static bool batteryTempDirty = false;
static int lastCellDelta = -1;
static bool cellDeltaDirty = false;

static bool mqttStatusRequested = false;
static unsigned long statusRequestTime = 0;

static StringBuffer<128> mqttPrefix;
// Response topic = prefix + client/<id>/response/<cmd> — external client ids
// are unbounded, keep headroom so format() never truncates mid-topic
static StringBuffer<192> currentResponseTopic;

// --- publish outbox --------------------------------------------------------
// PubSubClient publishes are synchronous QoS 0. Two failure modes used to
// lose data: a publish that failed mid-session was only retried when the
// value changed again, and the reconnect path fired ~20 publishes back to
// back, blocking loop() for seconds on the modem link. Failed publishes are
// queued here now, and everything queued is drained at a fixed pace from
// manageMQTT().
struct MqttOutboxEntry {
  char topic[112];
  char payload[56];
  bool retained;
};
static const uint8_t MQTT_OUTBOX_SIZE = 16;
static MqttOutboxEntry mqttOutbox[MQTT_OUTBOX_SIZE];
static uint8_t mqttOutboxHead = 0;
static uint8_t mqttOutboxCount = 0;
static unsigned long mqttOutboxLastDrainMs = 0;
static unsigned long mqttOutboxLastFailMs = 0;

static bool mqttEnqueue(const char* topic, const char* payload, bool retained) {
  if (!topic || !payload) return false;
  if (strlen(topic) >= sizeof(mqttOutbox[0].topic)) return false;
  // Long payloads (the GPS JSON) do not fit; their dirty flag covers them.
  if (strlen(payload) >= sizeof(mqttOutbox[0].payload)) return false;
  uint8_t slot;
  if (mqttOutboxCount < MQTT_OUTBOX_SIZE) {
    slot = (uint8_t)((mqttOutboxHead + mqttOutboxCount) % MQTT_OUTBOX_SIZE);
    mqttOutboxCount++;
  } else {
    // Full: drop the oldest entry. Metrics are last-value-wins, so the
    // freshest queued value matters more than the order.
    slot = mqttOutboxHead;
    mqttOutboxHead = (uint8_t)((mqttOutboxHead + 1) % MQTT_OUTBOX_SIZE);
  }
  strlcpy(mqttOutbox[slot].topic, topic, sizeof(mqttOutbox[slot].topic));
  strlcpy(mqttOutbox[slot].payload, payload, sizeof(mqttOutbox[slot].payload));
  mqttOutbox[slot].retained = retained;
  return true;
}

static void mqttDrainOutbox() {
  if (mqttOutboxCount == 0 || !mqttClient.connected()) return;
  unsigned long now = millis();
  if (now - mqttOutboxLastDrainMs < 15) return;  // pace: ~60 messages per second
  if (mqttOutboxLastFailMs != 0 && now - mqttOutboxLastFailMs < 1000) return;
  MqttOutboxEntry& e = mqttOutbox[mqttOutboxHead];
  if (mqttClient.publish(e.topic, (const uint8_t*)e.payload, strlen(e.payload), e.retained)) {
    mqttOutboxHead = (uint8_t)((mqttOutboxHead + 1) % MQTT_OUTBOX_SIZE);
    mqttOutboxCount--;
    mqttOutboxLastDrainMs = now;
    mqttOutboxLastFailMs = 0;
  } else {
    mqttOutboxLastFailMs = now;
  }
}

// Republish one stale metric per call. Every mqttUpdate*() clears its own
// dirty flag on success, so a fixed-order sweep converges on its own; the
// fail timestamp backs off for a second when the broker refuses writes.
static unsigned long mqttDirtyLastDrainMs = 0;
static unsigned long mqttDirtyLastFailMs = 0;

static void mqttDrainDirty() {
  if (!mqttClient.connected()) return;
  unsigned long now = millis();
  if (now - mqttDirtyLastDrainMs < 50) return;
  if (mqttDirtyLastFailMs != 0 && now - mqttDirtyLastFailMs < 1000) return;
  mqttDirtyLastDrainMs = now;
  if (socDirty && lastSOC >= 0) { mqttUpdateSOC(lastSOC); if (socDirty) mqttDirtyLastFailMs = now; return; }
  if (chargingDirty && lastChargingStateSet) { mqttUpdateCharging(lastIsCharging, lastChargerState); if (chargingDirty) mqttDirtyLastFailMs = now; return; }
  if (hvacDirty && lastHvacStateSet) { mqttUpdateHVAC(lastIsHvacOn); if (hvacDirty) mqttDirtyLastFailMs = now; return; }
  if (packDirty && lastPackVoltage >= 0) { mqttUpdatePackStatus(lastPackVoltage, lastPackCurrent); if (packDirty) mqttDirtyLastFailMs = now; return; }
  if (lockDirty && lastLockStateSet) { mqttUpdateLock(lastLockState == 1); if (lockDirty) mqttDirtyLastFailMs = now; return; }
  if (setpointDirty && lastSetpoint >= 0) { mqttUpdateHVACSetpoint(lastSetpoint); if (setpointDirty) mqttDirtyLastFailMs = now; return; }
  if (fanSpeedDirty && lastFanSpeed >= 0) { mqttUpdateFanSpeed(lastFanSpeed); if (fanSpeedDirty) mqttDirtyLastFailMs = now; return; }
  if (heatingDirty || coolingDirty) { mqttUpdateHeatingMode(lastHeating, lastCooling); if (heatingDirty || coolingDirty) mqttDirtyLastFailMs = now; return; }
  if (ventilationModeDirty) { mqttUpdateVentilationMode(lastVentilationMode); if (ventilationModeDirty) mqttDirtyLastFailMs = now; return; }
  if (doorsDirty && lastDoorsStateSet) { mqttUpdateDoors(lastDoorFL, lastDoorFR, lastDoorRL, lastDoorRR, lastDoorTrunk); if (doorsDirty) mqttDirtyLastFailMs = now; return; }
  if (sohDirty && lastSOH >= 0) { mqttUpdateSOH(lastSOH); if (sohDirty) mqttDirtyLastFailMs = now; return; }
  if (gidsDirty && lastGids >= 0) { mqttUpdateGids(lastGids); if (gidsDirty) mqttDirtyLastFailMs = now; return; }
  if (ambientDirty && lastAmbient > -90) { mqttUpdateAmbient(lastAmbient); if (ambientDirty) mqttDirtyLastFailMs = now; return; }
  if (vehicleOnDirty && lastVehicleOnSet) { mqttUpdateVehicleOn(lastVehicleOn); if (vehicleOnDirty) mqttDirtyLastFailMs = now; return; }
  if (headlightsDirty && lastHeadlightsSet) { mqttUpdateHeadlights(lastHeadlights); if (headlightsDirty) mqttDirtyLastFailMs = now; return; }
  if (cabinTempDirty && lastCabinTemp > -30) { mqttUpdateCabinTemp(lastCabinTemp); if (cabinTempDirty) mqttDirtyLastFailMs = now; return; }
  if (instrumentSocDirty && lastInstrumentSoc >= 0) { mqttUpdateInstrumentSoc(lastInstrumentSoc); if (instrumentSocDirty) mqttDirtyLastFailMs = now; return; }
  if (instrumentRangeDirty && lastInstrumentRange >= 0) { mqttUpdateInstrumentRange(lastInstrumentRange); if (instrumentRangeDirty) mqttDirtyLastFailMs = now; return; }
  if (batteryTempDirty && lastBatteryTemp > -90) { mqttUpdateBatteryTemp(lastBatteryTemp); if (batteryTempDirty) mqttDirtyLastFailMs = now; return; }
  if (cellDeltaDirty && lastCellDelta >= 0) { mqttUpdateCellDelta(lastCellDelta); if (cellDeltaDirty) mqttDirtyLastFailMs = now; return; }
  if (gpsDirty && lastGpsSet) { mqttUpdateGPS(lastGpsLat, lastGpsLon, lastGpsAlt, lastGpsSpd); if (gpsDirty) mqttDirtyLastFailMs = now; return; }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StringBuffer<64> msg;
  msg.copyFromData(payload, length);
  msg.trim();
  msg.toLowerCase();

  StringBuffer<128> topicStr;
  topicStr.copyFrom(topic);

  Serial.print("MQTT Command received on topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  Serial.println(msg.c_str());

  StringBuffer<80> clientPrefix;
  clientPrefix.format("%sclient/", mqttPrefix.c_str());
  
  // OVMS app sends commands to client/<clientid>/command/<commandid>
  if (topicStr.startsWith(clientPrefix.c_str())) {
    const char* clientInfo = topicStr.c_str() + clientPrefix.length(); // e.g. "someclient/command/someid"
    const char* cmdMarker = strstr(clientInfo, "/command/");
    
    if (cmdMarker != NULL) {
      size_t clientIdLen = cmdMarker - clientInfo;
      StringBuffer<64> clientId;
      // Only copy if there's something to copy to avoid issues with topics like "client//command"
      if (clientIdLen > 0) {
        clientId.copyFromData((const byte*)clientInfo, clientIdLen);
      }
      
      const char* commandId = cmdMarker + 9;
      
      // Prepare the response topic to reply to this specific command
      if (!clientId.isEmpty()) {
        currentResponseTopic.format("%sclient/%s/response/%s", mqttPrefix.c_str(), clientId.c_str(), commandId);
      }
      // The app sends "lock <pin>" or "unlock <pin>"
      // We need to check for the command prefix and ignore the pin.
      if (msg.startsWith(ovmsCmds.hornlights)) {
        handleMqttHornLights();
      } else if (msg.startsWith(ovmsCmds.rhlOff)) {
        handleMqttRhlOff();
      } else if (msg.startsWith(ovmsCmds.lights)) {
        handleMqttLights();
      } else if (msg.startsWith(ovmsCmds.lock)) {
        handleMqttLock();
      } else if (msg.startsWith(ovmsCmds.unlock)) {
        handleMqttUnlock();
      } else if (msg.startsWith(ovmsCmds.hvacOn)) {
        float setpoint = 0.0f;
        uint16_t minutes = 0;
        if (msg.length() > strlen(ovmsCmds.hvacOn)) {
            const char* args = msg.c_str() + strlen(ovmsCmds.hvacOn);
            setpoint = atof(args);
            const char* separator = strchr(args + 1, ' ');
            if (separator) minutes = (uint16_t)atoi(separator + 1);
        }
        handleMqttHvacOn(setpoint, minutes);
      } else if (msg.equals(ovmsCmds.hvacOff)) {
        handleMqttHvacOff();
      }
      else if (msg.equals(ovmsCmds.chargeOn)) {
        handleMqttChargeOn();
      }
      else if (msg.equals(ovmsCmds.chargeOff)) {
        handleMqttChargeOff();
      }
      else if (msg.equals(ovmsCmds.status)) {
        // Only queue the delayed status reply if the refresh actually started
        // (on busy the "Busy: ..." reply was already published above)
        if (handleMqttRefresh()) {
          mqttStatusRequested = true;
          statusRequestTime = millis();
          // Respond immediately, full status sent later in manageMQTT()
          mqttPublishStatus("Reading CAN bus...");
        }
      }
      else {
        StringBuffer<64> unknownMsg;
        unknownMsg.format("Unknown command: %s", msg.c_str());
        mqttPublishStatus(unknownMsg.c_str());
      }
    }
  }
}

boolean reconnectMQTT() {
  Serial.printf("[mqtt] reconnect via %s:%u\n", getMqttServer(), getMqttPort());
  // Transport: WiFi (TLS optional) when associated, else cellular modem (plain TCP)
  if (WiFi.status() == WL_CONNECTED) {
    if (getMqttTls()) mqttClient.setClient(espClientSecure);
    else mqttClient.setClient(espClient);
  } else if (modemMqttReady()) {
    Client* mc = modemTcpClient();
    if (!mc) return false;
    if (getMqttTls()) Serial.println("[modem] TLS not supported over modem, using plain TCP");
    mqttClient.setClient(*mc);
  } else {
    return false;
  }

  StringBuffer<32> clientId;
  clientId.format("LeafControl-%08X", (unsigned int)esp_random());
  
  StringBuffer<128> willTopic;
  willTopic.format("%smetric/s/v3/connected", mqttPrefix.c_str());
  
  bool connected = false;
  // Will at QoS 1 so an abnormal disconnect is actually delivered; broker
  // must keep the session to queue incoming commands while the board sleeps
  if (strlen(getMqttUser()) > 0) {
    connected = mqttClient.connect(clientId.c_str(), getMqttUser(), getMqttPassword(), willTopic.c_str(), 1, true, "no");
  } else {
    connected = mqttClient.connect(clientId.c_str(), "", "", willTopic.c_str(), 1, true, "no");
  }

  if (connected) {
    Serial.println("MQTT Connected!");
    
    // Mark as connected in Retained message (for OVMS app to see vehicle is online)
    mqttClient.publish(willTopic.c_str(), (const uint8_t*)"yes", 3, true);
    
    // Subscribe to OVMS commands from any client (QoS 1: broker retries
    // delivery if the board drops mid-command)
    StringBuffer<128> cmdTopic;
    cmdTopic.format("%sclient/+/command/+", mqttPrefix.c_str());
    mqttClient.subscribe(cmdTopic.c_str(), 1);
    
    if (getHassDiscoveryEnabled()) {
      // Retained configs persist on the broker — publish once per boot only
      static bool hassDiscoveryPublished = false;
      if (!hassDiscoveryPublished) {
        mqttPublishHassDiscovery(mqttClient, mqttPrefix);
        hassDiscoveryPublished = true;
      }
    }

    // Force sync latest known metrics on reconnect by marking them as dirty
    if (lastSOC >= 0) { socDirty = true; mqttUpdateSOC(lastSOC); }
    if (lastCabinTemp > -30) { cabinTempDirty = true; mqttUpdateCabinTemp(lastCabinTemp); }
    if (lastChargingStateSet) { chargingDirty = true; mqttUpdateCharging(lastIsCharging, lastChargerState); }
    if (lastHvacStateSet) { hvacDirty = true; mqttUpdateHVAC(lastIsHvacOn); }
    if (lastSetpoint >= 0) { setpointDirty = true; mqttUpdateHVACSetpoint(lastSetpoint); }
    if (lastFanSpeed >= 0) { fanSpeedDirty = true; mqttUpdateFanSpeed(lastFanSpeed); }
    if (lastHvacStateSet) { heatingDirty = true; coolingDirty = true; mqttUpdateHeatingMode(lastHeating, lastCooling); }
    if (lastHvacStateSet) { ventilationModeDirty = true; mqttUpdateVentilationMode(lastVentilationMode); }
    if (lastDoorsStateSet) { doorsDirty = true; mqttUpdateDoors(lastDoorFL, lastDoorFR, lastDoorRL, lastDoorRR, lastDoorTrunk); }
    if (lastLockStateSet) { lockDirty = true; mqttUpdateLock(lastLockState); }
    if (lastSOH >= 0) { sohDirty = true; mqttUpdateSOH(lastSOH); }
    if (lastGids >= 0) { gidsDirty = true; mqttUpdateGids(lastGids); }
    if (lastPackVoltage >= 0) { packDirty = true; mqttUpdatePackStatus(lastPackVoltage, lastPackCurrent); }
    if (lastAmbient > -90) { ambientDirty = true; mqttUpdateAmbient(lastAmbient); }
    if (lastVehicleOnSet) { vehicleOnDirty = true; mqttUpdateVehicleOn(lastVehicleOn); }
    if (lastHeadlightsSet) { headlightsDirty = true; mqttUpdateHeadlights(lastHeadlights); }
    if (lastGpsSet) { gpsDirty = true; mqttUpdateGPS(lastGpsLat, lastGpsLon, lastGpsAlt, lastGpsSpd); }
    if (lastOdometer > 0) mqttUpdateOdometer(lastOdometer);
    if (lastVin.length() == 17) mqttUpdateVin(lastVin.c_str());
    mqttUpdateHvacTimer(lastHvacTimer);
    
    return true;
  }
  Serial.printf("[mqtt] broker connect failed, state=%d\n", mqttClient.state());
  return false;
}

void setupMQTT() {
  mqttPrefix.format("ovms/%s/%s/", getMqttUser(), getMqttVehicleId());
  if (getMqttTls()) {
    espClientSecure.setInsecure(); // Skip certificate validation for simplicity
    espClientSecure.setHandshakeTimeout(5); // default 12s blocks loop() too long
    mqttClient.setClient(espClientSecure);
  } else {
    mqttClient.setClient(espClient);
  }
  mqttClient.setServer(getMqttServer(), getMqttPort());
  mqttClient.setSocketTimeout(5); // default 15s blocks loop() during outages

  // OVMS messages have longer topics, so enlarging buffer is recommended
  mqttClient.setBufferSize(1024);
  mqttClient.setCallback(mqttCallback);
}

bool mqttIsConnected() {
  return strlen(getMqttServer()) > 0 && mqttClient.connected();
}

void mqttDisconnect() {
  if (mqttClient.connected()) mqttClient.disconnect();
}

void mqttGoingOffline() {
  // Called right before deep sleep. Publish the retained offline marker
  // synchronously (the will would do it anyway, but only after the TCP
  // reset is noticed — minutes later on some brokers/clients).
  if (!mqttClient.connected()) return;
  StringBuffer<128> willTopic;
  willTopic.format("%smetric/s/v3/connected", mqttPrefix.c_str());
  mqttClient.publish(willTopic.c_str(), (const uint8_t*)"no", 2, true);
  // Give the socket a moment to actually flush before power drops
  mqttClient.loop();
  delay(50);
  mqttClient.disconnect();
}

void manageMQTT() {
  if (getRemoteTransport() != RemoteTransport::MQTT) {
    mqttDisconnect();
    return;
  }
  if (strlen(getMqttServer()) == 0) {
    return; // Don't try to connect if MQTT server is not configured
  }

  bool linkUp = (WiFi.status() == WL_CONNECTED) || modemMqttReady();
  if (linkUp) {
    if (!mqttClient.connected()) {
      // First attempt runs immediately after boot (lastMqttReconnectAttempt==0
      // would otherwise delay it by 30 s)
      if (lastMqttReconnectAttempt == 0 ||
          millis() - lastMqttReconnectAttempt > 30000) {
        lastMqttReconnectAttempt = millis();
        if (reconnectMQTT()) {
          lastMqttReconnectAttempt = 0;
        }
      }
    } else {
      mqttClient.loop();
    }
  }

  // Retry queued metric publishes and refresh any metric whose last publish
  // failed (outbox is drained even while a reconnect is pending, once linked)
  if (mqttClient.connected()) {
    mqttDrainOutbox();
    mqttDrainDirty();
  }

  // Send status with delay if requested
  if (mqttStatusRequested && (millis() - statusRequestTime > 1500)) {
    StringBuffer<16> socStr;
    if (lastSOC >= 0) {
      socStr.format("%.1f%%", lastSOC);
    } else {
      socStr.copyFrom("No data");
    }
    
    StringBuffer<16> tempStr;
    if (lastCabinTemp > -30) {
      tempStr.format("%.1fC", lastCabinTemp);
    } else {
      tempStr.copyFrom("No data");
    }
    
    StringBuffer<128> statusMsg;
    statusMsg.format("Battery: %s | Temp: %s", socStr.c_str(), tempStr.c_str());
    mqttPublishStatus(statusMsg.c_str());
    
    mqttStatusRequested = false;
  }
}

bool mqttPublishMetric(const char* metric, float value, const char* format = "%.1f") {
  if (mqttClient.connected()) {
    StringBuffer<128> topic;
    topic.format("%s%s", mqttPrefix.c_str(), metric);
    StringBuffer<16> payload;
    payload.format(format, value);
    // retained = true
    return mqttClient.publish(topic.c_str(), (const uint8_t*)payload.c_str(), payload.length(), true);
  }
  return false;
}

bool mqttPublishMetricStr(const char* metric, const char* payload) {
  if (mqttClient.connected()) {
    StringBuffer<128> topic;
    topic.format("%s%s", mqttPrefix.c_str(), metric);
    return mqttClient.publish(topic.c_str(), (const uint8_t*)payload, strlen(payload), true);
  }
  return false;
}

void mqttUpdateCharging(bool isCharging, ChargerState state) {
  if (lastChargingStateSet && lastIsCharging == isCharging && lastChargerState == state && !chargingDirty) {
    return;
  }
  lastChargingStateSet = true;
  lastIsCharging = isCharging;
  lastChargerState = state;

  bool success = true;
  success &= mqttPublishMetricStr(ovmsMetrics.chargingActive, isCharging ? "yes" : "no");
  switch (state) {
    case ChargerState::CHARGING: success &= mqttPublishMetricStr(ovmsMetrics.chargingState, "charging"); break;
    case ChargerState::FINISHED: success &= mqttPublishMetricStr(ovmsMetrics.chargingState, "done"); break;
    case ChargerState::INTERRUPTED: success &= mqttPublishMetricStr(ovmsMetrics.chargingState, "stopped"); break;
    case ChargerState::WAITING: success &= mqttPublishMetricStr(ovmsMetrics.chargingState, "wait"); break;
    case ChargerState::IDLE:
    default: success &= mqttPublishMetricStr(ovmsMetrics.chargingState, ""); break;
  }
  chargingDirty = !success;
}

void mqttUpdateHVAC(bool isOn) {
  if (lastHvacStateSet && lastIsHvacOn == isOn && !hvacDirty) return;
  lastHvacStateSet = true;
  lastIsHvacOn = isOn;
  hvacDirty = !mqttPublishMetricStr(ovmsMetrics.hvacActive, isOn ? "yes" : "no");
}

void mqttUpdateHVACSetpoint(float setpoint) {
  if (lastSetpoint == setpoint && !setpointDirty) return;
  lastSetpoint = setpoint;
  setpointDirty = !mqttPublishMetric(ovmsMetrics.hvacSetpoint, setpoint);
}

void mqttUpdateFanSpeed(float speed) {
  if (lastFanSpeed == speed && !fanSpeedDirty) return;
  lastFanSpeed = speed;
  fanSpeedDirty = !mqttPublishMetric(ovmsMetrics.fanSpeed, speed);
}

void mqttUpdateHeatingMode(bool heating, bool cooling) {
  if (lastHeating == heating && !heatingDirty && lastCooling == cooling && !coolingDirty) return;
  lastHeating = heating;
  lastCooling = cooling;
  heatingDirty = !mqttPublishMetricStr(ovmsMetrics.heating, heating ? "yes" : "no");
  coolingDirty = !mqttPublishMetricStr(ovmsMetrics.cooling, cooling ? "yes" : "no");
}

const char* ventilationModeToString(VentilationMode mode) {
  switch (mode) {
    case VentilationMode::OFF: return "off";
    case VentilationMode::FACE: return "face";
    case VentilationMode::FACE_FEET: return "face|feet";
    case VentilationMode::FEET: return "feet";
    case VentilationMode::WINDSCREEN_FEET: return "windscreen|feet";
    case VentilationMode::WINDSCREEN: return "windscreen";
    case VentilationMode::UNKNOWN:
    default: return "";
  }
}

void mqttUpdateVentilationMode(VentilationMode mode) {
  if (lastVentilationMode == mode && !ventilationModeDirty) return;
  lastVentilationMode = mode;
  ventilationModeDirty = !mqttPublishMetricStr(ovmsMetrics.ventilationMode, ventilationModeToString(mode));
}

void mqttUpdateSOC(float soc) {
  if (lastSOC == soc && !socDirty) return;
  lastSOC = soc;
  socDirty = !mqttPublishMetric(ovmsMetrics.soc, soc);
}

void mqttUpdateCabinTemp(float temp) {
  if (lastCabinTemp == temp && !cabinTempDirty) return;
  lastCabinTemp = temp;
  cabinTempDirty = !mqttPublishMetric(ovmsMetrics.cabinTemp, temp);
}

void mqttUpdateDoors(bool fl, bool fr, bool rl, bool rr, bool trunk) {
  static unsigned long stableSince = 0;
  static bool candFL = false, candFR = false, candRL = false, candRR = false, candTrunk = false;
  static bool candSet = false;

  if (lastDoorsStateSet && lastDoorFL == fl && lastDoorFR == fr &&
      lastDoorRL == rl && lastDoorRR == rr && lastDoorTrunk == trunk && !doorsDirty) {
    candSet = false;
    return;
  }

  bool isRetry = (lastDoorsStateSet && lastDoorFL == fl && lastDoorFR == fr &&
                  lastDoorRL == rl && lastDoorRR == rr && lastDoorTrunk == trunk);

  if (!isRetry && (!candSet || candFL != fl || candFR != fr || candRL != rl || candRR != rr || candTrunk != trunk)) {
    candFL = fl; candFR = fr; candRL = rl; candRR = rr; candTrunk = trunk;
    stableSince = millis();
    candSet = true;
    return;
  }

  if (isRetry || millis() - stableSince >= 250) {
    bool outFL = isRetry ? fl : candFL;
    bool outFR = isRetry ? fr : candFR;
    bool outRL = isRetry ? rl : candRL;
    bool outRR = isRetry ? rr : candRR;
    bool outTrunk = isRetry ? trunk : candTrunk;

    bool success = true;
    success &= mqttPublishMetricStr(ovmsMetrics.doorFL, outFL ? "yes" : "no");
    success &= mqttPublishMetricStr(ovmsMetrics.doorFR, outFR ? "yes" : "no");
    success &= mqttPublishMetricStr(ovmsMetrics.doorRL, outRL ? "yes" : "no");
    success &= mqttPublishMetricStr(ovmsMetrics.doorRR, outRR ? "yes" : "no");
    success &= mqttPublishMetricStr(ovmsMetrics.doorTrunk, outTrunk ? "yes" : "no");

    lastDoorsStateSet = true;
    lastDoorFL = outFL; lastDoorFR = outFR; lastDoorRL = outRL; lastDoorRR = outRR; lastDoorTrunk = outTrunk;
    doorsDirty = !success;
    candSet = false;
  }
}

void mqttUpdateLock(bool locked) {
  if (lastLockStateSet && lastLockState == locked && !lockDirty) return;
  lastLockStateSet = true;
  lastLockState = locked;
  lockDirty = !mqttPublishMetricStr(ovmsMetrics.locked, locked ? "yes" : "no");
}

void mqttUpdateSOH(int soh) {
  if (lastSOH == soh && !sohDirty) return;
  lastSOH = soh;
  sohDirty = !mqttPublishMetric(ovmsMetrics.soh, (float)soh, "%.0f");
}

void mqttUpdateGids(int gids) {
  if (lastGids == gids && !gidsDirty) return;
  lastGids = gids;
  gidsDirty = !mqttPublishMetric(ovmsMetrics.gids, (float)gids, "%.0f");
}

void mqttUpdatePackStatus(float voltage, float current) {
  if (lastPackVoltage == voltage && lastPackCurrent == current && !packDirty) return;
  lastPackVoltage = voltage;
  lastPackCurrent = current;
  bool ok = mqttPublishMetric(ovmsMetrics.packVoltage, voltage, "%.1f");
  ok &= mqttPublishMetric(ovmsMetrics.packCurrent, current, "%.1f");
  ok &= mqttPublishMetric(ovmsMetrics.packPower, voltage * current / 1000.0f, "%.1f");
  packDirty = !ok;
}

void mqttUpdateOdometer(uint32_t km) {
  lastOdometer = km;
  mqttPublishMetric(ovmsMetrics.odometer, (float)km, "%.0f");
}

void mqttUpdateVin(const char* vin) {
  lastVin = vin ? vin : "";
  mqttPublishMetricStr(ovmsMetrics.vin, vin);
}

void mqttUpdateHvacTimer(uint16_t minutes) {
  lastHvacTimer = minutes;
  mqttPublishMetric(ovmsMetrics.hvacTimer, (float)minutes, "%.0f");
}

void mqttUpdateAmbient(float ambientTemp) {
  if (lastAmbient == ambientTemp && !ambientDirty) return;
  lastAmbient = ambientTemp;
  ambientDirty = !mqttPublishMetric(ovmsMetrics.ambientTemp, ambientTemp, "%.1f");
}

void mqttUpdateInstrumentSoc(float soc) {
  if (soc == lastInstrumentSoc && !instrumentSocDirty) return;
  lastInstrumentSoc = soc;
  instrumentSocDirty = !mqttPublishMetric(ovmsMetrics.socInstrument, soc, "%.1f");
}

void mqttUpdateInstrumentRange(int km) {
  if (km == lastInstrumentRange && !instrumentRangeDirty) return;
  lastInstrumentRange = km;
  instrumentRangeDirty = !mqttPublishMetric(ovmsMetrics.rangeInstrument, (float)km, "%.0f");
}

void mqttUpdateBatteryTemp(float temp) {
  if (temp == lastBatteryTemp && !batteryTempDirty) return;
  lastBatteryTemp = temp;
  batteryTempDirty = !mqttPublishMetric(ovmsMetrics.batteryTemp, temp, "%.1f");
}

void mqttUpdateCellDelta(int mv) {
  if (mv == lastCellDelta && !cellDeltaDirty) return;
  lastCellDelta = mv;
  cellDeltaDirty = !mqttPublishMetric(ovmsMetrics.cellDelta, mv / 1000.0f, "%.3f");
}

void mqttUpdateVehicleOn(bool on) {
  if (lastVehicleOnSet && lastVehicleOn == on && !vehicleOnDirty) return;
  lastVehicleOnSet = true;
  lastVehicleOn = on;
  vehicleOnDirty = !mqttPublishMetricStr(ovmsMetrics.vehicleOn, on ? "yes" : "no");
}

void mqttUpdateHeadlights(bool on) {
  if (lastHeadlightsSet && lastHeadlights == on && !headlightsDirty) return;
  lastHeadlightsSet = true;
  lastHeadlights = on;
  headlightsDirty = !mqttPublishMetricStr(ovmsMetrics.headlights, on ? "yes" : "no");
}

void mqttUpdateGPS(float lat, float lon, float alt, float speed) {
  lastGpsSet = true;
  lastGpsLat = lat; lastGpsLon = lon; lastGpsAlt = alt; lastGpsSpd = speed;
  StringBuffer<160> payload;
  payload.format("{\"lat\":%.5f,\"lon\":%.5f,\"alt\":%.0f,\"spd\":%.1f,\"ts\":%lu}",
                 lat, lon, alt, speed, (unsigned long)(millis() / 1000UL));
  gpsDirty = !mqttPublishMetricStr(ovmsMetrics.location, payload.c_str());
}

void mqttPublishStatus(const char* msg) {
  if (mqttClient.connected() && !currentResponseTopic.isEmpty()) {
    mqttClient.publish(currentResponseTopic.c_str(), msg);
    
    // Once the reading CAN bus status returns, we clear the response topic
    if (!mqttStatusRequested) {
      currentResponseTopic.clear(); 
    }
  }
}
