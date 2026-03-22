/**
 * @file CypherNode.ino
 * @author Team Cypher-Z
 * @brief Optimized Dynamic Smart Home Server with Active-Low Support & Command Queue. Update functionality
 * @version 7.7.1 (Optimized & Improved)
 * @date 2026-03-12
 */

// Enable Sensors
#define ENABLE_DHT
// #define ENABLE_VAC // when use voltage sensor then uncomment it

#include "Sensors.h"

#include <time.h>  // for time

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <ESPWiFiManager.h>
#include <LittleFS.h>
#include <vector>

#include "WebPage.h"

#define ENABLE_DATABASE
#define ENABLE_USER_AUTH

#include <FirebaseClient.h>
#include <WiFiClientSecure.h>

// ==========================================================
// Firebase Configuration
// ==========================================================
#define FIREBASE_API_KEY "AIzaSyDbH7maOdAXZZlVs9kxb3Kc0vxqAVXUHHc"
#define FIREBASE_DATABASE_URL "https://cyphernode-27a24-default-rtdb.asia-southeast1.firebasedatabase.app"
// Server login credentials (for Firebase authentication)
#define ESP_USER_EMAIL "info.naxtechhome@gmail.com"
#define ESP_USER_PASSWORD "pass@root_server"

// ==========================================================
// HTTP Authentication Credentials (for Web UI & API)
// ==========================================================
const char* HTTP_USERNAME = "CypherNode";
const char* HTTP_PASSWORD = "CypherZ_team";
const String API_KEY = "d3fau1t";  // Optional API key for header-based auth

// ==========================================================
// Global Objects
// ==========================================================
WebServer server(80);
WiFiManager wifiManager("CypherNode", "password");

// Firebase secure clients
WiFiClientSecure sslStates;
WiFiClientSecure sslCmds;
WiFiClientSecure sslPush;
using AsyncClient = AsyncClientClass;
AsyncClient aClientStates(sslStates);
AsyncClient aClientCmds(sslCmds);
AsyncClient aClientPush(sslPush);

// Automation stream client
WiFiClientSecure sslAuto;
using AsyncClient = AsyncClientClass;
AsyncClient aClientAuto(sslAuto);

FirebaseApp app;
RealtimeDatabase Database;
UserAuth userAuth(FIREBASE_API_KEY, ESP_USER_EMAIL, ESP_USER_PASSWORD);

bool isFirebaseConnected = false;

// wifimanager lib flag
bool apModeStarted = false;
bool connectionHandled = false;

// Firebase database paths
const String basePath = "/CypherNode/states";
const String configPath = "/CypherNode/config";
const String cmdsPath = "/CypherNode/commands";
const String logsPath = "/CypherNode/logs";
const String togglePath = "/CypherNode/commands/toggle/";
const String healthPath = "/CypherNode/health/pulse";
const String temp_alert = "/CypherNode/alerts/high_temp";

// System constants
const unsigned long DEBOUNCE_DELAY = 50;         // for physical switch debounce
const unsigned long HEARTBEAT_INTERVAL = 20000;  // 20 seconds

// Watchdog Timer Variables
unsigned long lastWifiRetryTime = 0;
const unsigned long wifiRetryInterval = 60000;

// Alert Flag
bool highTempAlertSent = false;

// Heartbeat
unsigned long lastHeartbeatTime = 0;
long heartbeatPulse = 0;

// ==========================================================
// Device Structure
// ==========================================================
struct Device {
  String loadID;
  String loadName;
  String loadType;
  int loadGPIO;
  bool hasPhysicalSwitch;
  int switchGPIO;
  bool activeHigh;       // true = HIGH turns device ON, false = LOW turns device ON
  bool state;            // current logical state (true = ON, false = OFF)
  bool lastSwitchState;  // last raw reading from physical switch
  unsigned long lastDebounceTime = 0;
  bool stableSwitchState;  // debounced switch state
};

// ==========================================================
// Automation rule list
// ==========================================================
struct AutoRule {
  String id;
  String type;
  float triggerAbove;
  String tempCondition = "above"; // for settigs temp rule type
  float hysteresis = 1.0; // default value 1 (newly added)
  int hour;
  int minute;
  String loadID;
  bool active;

  // new variable for handle new rules
  bool actionTurnOn;      
  bool lastConditionMet;  
  int lastTriggeredDay;
};

std::vector<Device> devices;
std::vector<AutoRule> activeRules;

// ==========================================================
// Async Flag System (Delayed Flash Write)
// ==========================================================
bool stateNeedsSave = false;
unsigned long lastStateSaveTime = 0;

// NTP setup
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 6 * 3600;
const int daylightOffset_sec = 0;

// ==========================================================
// Forward Declarations
// ==========================================================
void applyHardwareState(const Device& dev);
void saveStatesToFile();
void loadStatesFromFile();
void saveConfigToFile();
void loadConfigFromFile();
void pushCallback(AsyncResult& aResult);
void pushSystemLog(const String& message);
void syncAllToFirebaseAtomic();
void updateFirebaseState(const String& loadID, bool state);
void statesStreamCallback(AsyncResult& aResult);
void cmdsStreamCallback(AsyncResult& aResult);
void processToggleCommand(const String& targetID, int targetState);
void processDeleteCommand(const String& targetID);
void processAddCommand(const String& targetID, const String& valStr);
void processSystemCommand(const String& command, const String& valStr);
bool isAuthorizedAPI();
void handleRoot();
void handleGetConfig();
void handleSaveConfig();
void handleGetState();
void handleUpdateState();
void handleDeleteLoad();
void handlePhysicalSwitches();
void initCloudServices();
void saveAutomationsToFile();
void loadAutomationsFromFile();
void autoStreamCallback(AsyncResult& aResult);


// ==========================================================
// Hardware Control Helpers
// ==========================================================

/**
 * @brief Applies the logical state to the physical GPIO pin considering activeHigh polarity.
 * @param dev Reference to the device.
 */
void applyHardwareState(const Device& dev) {
  bool pinState = (dev.state == dev.activeHigh);  // if activeHigh is true, state true => HIGH; if activeHigh false, state true => LOW
  digitalWrite(dev.loadGPIO, pinState ? HIGH : LOW);
}

// ==========================================================
// State & Configuration Persistence (LittleFS)
// ==========================================================

/**
 * @brief Saves current device states to /states.json in LittleFS.
 */
void saveStatesToFile() {
  DynamicJsonDocument doc(1024);
  for (const auto& dev : devices) {
    doc[dev.loadID] = dev.state ? 1 : 0;
  }
  File file = LittleFS.open("/states.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

/**
 * @brief Loads device states from /states.json and applies them.
 *        Called only during initial boot (if no config existed).
 */
void loadStatesFromFile() {
  if (!LittleFS.exists("/states.json")) return;

  File file = LittleFS.open("/states.json", "r");
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (!error) {
    JsonObject root = doc.as<JsonObject>();
    for (auto& dev : devices) {
      if (root.containsKey(dev.loadID)) {
        dev.state = (root[dev.loadID].as<int>() == 1);
        applyHardwareState(dev);
      }
    }
  }
}

/**
 * @brief Saves device configuration to /config.json.
 */
void saveConfigToFile() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();

  for (auto& dev : devices) {
    JsonObject obj = arr.createNestedObject();
    obj["loadID"] = dev.loadID;
    obj["loadName"] = dev.loadName;
    obj["loadType"] = dev.loadType;
    obj["loadGPIO"] = dev.loadGPIO;
    obj["hasPhysicalSwitch"] = dev.hasPhysicalSwitch;
    obj["switchGPIO"] = dev.switchGPIO;
    obj["activeHigh"] = dev.activeHigh;
  }

  File file = LittleFS.open("/config.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

/**
 * @brief Loads device configuration from /config.json.
 *        Preserves existing states of devices that were already known.
 */
void loadConfigFromFile() {
  if (!LittleFS.exists("/config.json")) return;

  File file = LittleFS.open("/config.json", "r");
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) return;

  std::vector<Device> oldDevices = std::move(devices);  // save old states
  devices.clear();

  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    Device dev;
    dev.loadID = obj["loadID"].as<String>();
    dev.loadName = obj["loadName"].as<String>();
    dev.loadType = obj.containsKey("loadType") ? obj["loadType"].as<String>() : "light";
    dev.loadGPIO = obj["loadGPIO"].as<int>();
    dev.hasPhysicalSwitch = obj["hasPhysicalSwitch"].as<bool>();
    dev.switchGPIO = obj["switchGPIO"].as<int>();
    dev.activeHigh = obj.containsKey("activeHigh") ? obj["activeHigh"].as<bool>() : true;

    // Try to preserve previous state
    bool found = false;
    for (const auto& old : oldDevices) {
      if (old.loadID == dev.loadID) {
        dev.state = old.state;
        dev.lastSwitchState = old.lastSwitchState;
        dev.stableSwitchState = old.stableSwitchState;
        found = true;
        break;
      }
    }
    if (!found) {
      dev.state = false;  // default OFF
    }

    pinMode(dev.loadGPIO, OUTPUT);
    applyHardwareState(dev);

    if (dev.hasPhysicalSwitch) {
      pinMode(dev.switchGPIO, INPUT_PULLUP);
      if (!found) {
        dev.lastSwitchState = digitalRead(dev.switchGPIO);
        dev.stableSwitchState = dev.lastSwitchState;
      }
    }
    devices.push_back(dev);
  }

  // If no old devices existed (first boot), load states from file
  if (oldDevices.empty()) {
    loadStatesFromFile();
  }
}

// ==========================================================
// Firebase Helpers & Callbacks
// ==========================================================

/**
 * @brief Generic callback for Firebase operations (logs errors).
 */
void pushCallback(AsyncResult& aResult) {
  if (aResult.isError()) {
    Serial.println("Firebase Error: " + aResult.error().message());
  }
}

/**
 * @brief Pushes a system log message to Firebase.
 * @param message Log text.
 */
void pushSystemLog(const String& message) {
  if (!isFirebaseConnected) return;

  DynamicJsonDocument doc(256);
  JsonObject tsObj = doc.createNestedObject("timestamp");
  tsObj[".sv"] = "timestamp";
  doc["message"] = message;

  String payload;
  serializeJson(doc, payload);
  Database.push<object_t>(aClientPush, logsPath, object_t(payload), pushCallback, "logTask");
}

/**
 * @brief Atomically writes both config and states to Firebase under /CypherNode.
 *        Used at startup and after configuration changes.
 */
void syncAllToFirebaseAtomic() {
  if (!isFirebaseConnected) return;

  DynamicJsonDocument configDoc(2048);
  JsonObject configObj = configDoc.to<JsonObject>();
  for (auto& dev : devices) {
    JsonObject d = configObj.createNestedObject(dev.loadID);
    d["loadName"] = dev.loadName;
    d["loadType"] = dev.loadType;
    d["loadGPIO"] = dev.loadGPIO;
    d["hasPhysicalSwitch"] = dev.hasPhysicalSwitch;
    d["switchGPIO"] = dev.switchGPIO;
    d["activeHigh"] = dev.activeHigh;
  }

  DynamicJsonDocument stateDoc(1024);
  JsonObject stateObj = stateDoc.to<JsonObject>();
  for (const auto& dev : devices) {
    stateObj[dev.loadID] = dev.state ? 1 : 0;
  }

  DynamicJsonDocument payloadDoc(3072);
  payloadDoc["config"] = configObj;
  payloadDoc["states"] = stateObj;

  String jsonStr;
  serializeJson(payloadDoc, jsonStr);

  Database.update<object_t>(aClientPush, "/CypherNode", object_t(jsonStr), pushCallback, "atomicSyncAll");
}

/**
 * @brief Updates a single device state in Firebase (under states/).
 * @param loadID Device identifier.
 * @param state  New logical state.
 */
void updateFirebaseState(const String& loadID, bool state) {
  if (isFirebaseConnected) {
    String path = basePath + "/" + loadID;
    Database.set<int>(aClientPush, path, state ? 1 : 0, pushCallback, "pushState");
  }
}

/**
 * @brief Stream callback for listening to state changes from Firebase (cloud → ESP).
 *        When a state changes in the cloud, apply it locally.
 */
void statesStreamCallback(AsyncResult& aResult) {
  if (aResult.isError() || !aResult.available()) return;
  RealtimeDatabaseResult& RTDB = aResult.to<RealtimeDatabaseResult>();

  if (!RTDB.isStream()) return;

  String path = RTDB.dataPath();
  if (path.length() <= 1) return;  // ignore root

  String valStr = RTDB.to<String>();
  if (valStr == "null") return;

  String loadID = path.substring(1);  // remove leading '/'
  int newState = valStr.toInt();

  for (auto& dev : devices) {
    if (dev.loadID == loadID && dev.state != (newState == 1)) {
      dev.state = (newState == 1);
      applyHardwareState(dev);
      saveStatesToFile();
      break;
    }
  }
}

/**
 * @brief Processes a toggle command (single device).
 * @param targetID    Device ID.
 * @param targetState Desired state (1 = ON, 0 = OFF).
 */
void processToggleCommand(const String& targetID, int targetState) {
  for (auto& dev : devices) {
    if (dev.loadID == targetID) {
      // Apply new state
      dev.state = (targetState == 1);
      applyHardwareState(dev);
      saveStatesToFile();

      // Report back to Firebase (so UI updates)
      updateFirebaseState(dev.loadID, dev.state);

      // Remove command from queue (acknowledgment)
      Database.remove(aClientPush, togglePath + targetID, pushCallback, "ackToggle");

      // Serial.println("✓ Executed toggle for: " + targetID);
      break;
    }
  }
}

/**
 * @brief Processes a delete command.
 * @param targetID Device ID to remove.
 */
void processDeleteCommand(const String& targetID) {
  auto it = std::find_if(devices.begin(), devices.end(),
                         [&](const Device& d) {
                           return d.loadID == targetID;
                         });
  if (it == devices.end()) return;

  devices.erase(it);
  saveConfigToFile();
  saveStatesToFile();

  // Atomically remove from Firebase config and states
  String jsonPayload = "{\"states/" + targetID + "\":null, \"config/" + targetID + "\":null, \"commands/delete/" + targetID + "\":null}";
  Database.update<object_t>(aClientPush, "/CypherNode", object_t(jsonPayload), pushCallback, "appDelAtomic");

  pushSystemLog("Device " + targetID + " removed.");
  // Serial.println("✓ Executed delete for: " + targetID);
}

/**
 * @brief Processes an add command.
 * @param targetID New device ID.
 * @param valStr   JSON string containing device configuration.
 */
void processAddCommand(const String& targetID, const String& valStr) {
  // Check if device already exists
  if (std::any_of(devices.begin(), devices.end(),
                  [&](const Device& d) {
                    return d.loadID == targetID;
                  })) {
    // Already exists – just remove the command and return
    Database.remove(aClientPush, "/CypherNode/commands/add/" + targetID, pushCallback, "ackAdd");
    return;
  }

  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, valStr);
  if (error) {
    Database.remove(aClientPush, "/CypherNode/commands/add/" + targetID, pushCallback, "ackAdd");
    return;
  }

  Device dev;
  dev.loadID = targetID;
  dev.loadName = doc["loadName"].as<String>();
  dev.loadType = doc["loadType"].as<String>();
  dev.loadGPIO = doc["loadGPIO"].as<int>();
  dev.hasPhysicalSwitch = doc["hasPhysicalSwitch"].as<bool>();
  dev.switchGPIO = doc["switchGPIO"].as<int>();
  dev.activeHigh = doc["activeHigh"].as<bool>();
  dev.state = false;
  dev.stableSwitchState = false;

  pinMode(dev.loadGPIO, OUTPUT);
  applyHardwareState(dev);

  if (dev.hasPhysicalSwitch) {
    pinMode(dev.switchGPIO, INPUT_PULLUP);
    dev.lastSwitchState = digitalRead(dev.switchGPIO);
    dev.stableSwitchState = dev.lastSwitchState;
  }

  devices.push_back(dev);
  saveConfigToFile();
  saveStatesToFile();

  // Atomically write config and initial state to Firebase
  String jsonPayload = "{\"states/" + targetID + "\":0, \"config/" + targetID + "\":" + valStr + "}";
  Database.update<object_t>(aClientPush, "/CypherNode", object_t(jsonPayload), pushCallback, "addAtomic");

  pushSystemLog("New device " + targetID + " added.");
  // Serial.println("✓ Executed add for: " + targetID);

  // Remove command from queue
  Database.remove(aClientPush, "/CypherNode/commands/add/" + targetID, pushCallback, "ackAdd");
}

/**
 * @brief Processes a system command (e.g., reboot).
 * @param command Command name.
 * @param valStr  Value (truthy to execute).
 */
void processSystemCommand(const String& command, const String& valStr) {
  bool execute = (valStr == "true" || valStr.toInt() == 1);
  if (!execute) return;

  Database.remove(aClientPush, "/CypherNode/commands/system/" + command, pushCallback, "ackSystem");

  if (command == "reboot") {
    pushSystemLog("Remote reboot initiated.");
    delay(1000);
    ESP.restart();
  }
}

/**
 * @brief Stream callback for the commands queue. Handles both nested JSON updates
 *        and individual sub-path updates.
 */
void cmdsStreamCallback(AsyncResult& aResult) {
  if (aResult.isError() || !aResult.available()) return;
  RealtimeDatabaseResult& RTDB = aResult.to<RealtimeDatabaseResult>();

  if (!RTDB.isStream()) return;

  String path = RTDB.dataPath();
  String valStr = RTDB.to<String>();
  if (valStr == "null") return;

  // Serial.println("\n[CMDS] Path: " + path);
  // Serial.println("[CMDS] Data: " + valStr);

  // Case 1: Full update at "/" (contains nested objects)
  if (path == "/") {
    DynamicJsonDocument rootDoc(2048);
    DeserializationError err = deserializeJson(rootDoc, valStr);
    if (err) return;

    JsonObject root = rootDoc.as<JsonObject>();

    // Toggle commands
    if (root.containsKey("toggle")) {
      JsonObject toggles = root["toggle"];
      for (JsonPair kv : toggles) {
        processToggleCommand(kv.key().c_str(), kv.value().as<int>());
      }
    }
    // Delete commands
    if (root.containsKey("delete")) {
      JsonObject deletes = root["delete"];
      for (JsonPair kv : deletes) {
        if (kv.value().as<bool>() || kv.value().as<int>() == 1) {
          processDeleteCommand(kv.key().c_str());
        }
      }
    }
    // Add commands
    if (root.containsKey("add")) {
      JsonObject adds = root["add"];
      for (JsonPair kv : adds) {
        String jsonStr;
        serializeJson(kv.value(), jsonStr);
        processAddCommand(kv.key().c_str(), jsonStr);
      }
    }
    // System commands
    if (root.containsKey("system")) {
      JsonObject systems = root["system"];
      for (JsonPair kv : systems) {
        processSystemCommand(kv.key().c_str(), kv.value().as<String>());
      }
    }
  } else if (path.startsWith("/toggle/")) {
    String targetID = path.substring(8);
    processToggleCommand(targetID, valStr.toInt());
  } else if (path.startsWith("/delete/")) {
    String targetID = path.substring(8);
    if (valStr.toInt() == 1 || valStr == "true") {
      processDeleteCommand(targetID);
    }
  } else if (path.startsWith("/add/")) {
    String targetID = path.substring(5);
    processAddCommand(targetID, valStr);
  } else if (path.startsWith("/system/")) {
    String command = path.substring(8);
    processSystemCommand(command, valStr);
  }
}

// ==========================================================
// HTTP Authentication Helper
// ==========================================================

/**
 * @brief Checks if the incoming HTTP request is authorized.
 *        Accepts either Basic Auth or X-API-Key header.
 * @return true if authorized, false otherwise.
 */
bool isAuthorizedAPI() {
  if (server.hasHeader("x-api-key") && server.header("x-api-key") == API_KEY) {
    return true;
  }
  if (server.authenticate(HTTP_USERNAME, HTTP_PASSWORD)) {
    return true;
  }
  return false;
}

// ==========================================================
// HTTP Request Handlers
// ==========================================================

/**
 * @brief Serves the main web interface (compressed HTML).
 */
void handleRoot() {
  if (!isAuthorizedAPI()) {
    return server.requestAuthentication();
  }
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (const char*)index_html_gz, index_html_gz_len);
}

/**
 * @brief Returns the current device configuration as JSON.
 */
void handleGetConfig() {
  if (!isAuthorizedAPI()) return server.send(401, "text/plain", "Unauthorized");

  File file = LittleFS.open("/config.json", "r");
  if (file) {
    server.streamFile(file, "application/json");
    file.close();
  } else {
    server.send(200, "application/json", "[]");
  }
}

/**
 * @brief Saves a new configuration received in the request body.
 *        Reloads devices and syncs to Firebase.
 */
void handleSaveConfig() {
  if (!isAuthorizedAPI()) return server.send(401, "text/plain", "Unauthorized");
  if (!server.hasArg("plain")) return server.send(400, "text/plain", "Body not received");

  // Write new config file
  File file = LittleFS.open("/config.json", "w");
  file.print(server.arg("plain"));
  file.close();

  // Reload devices (preserves states of matching IDs)
  loadConfigFromFile();

  // Push everything to Firebase
  syncAllToFirebaseAtomic();

  server.send(200, "text/plain", "OK");
}

/**
 * @brief Returns the current device states as JSON.
 */
void handleGetState() {
  if (!isAuthorizedAPI()) return server.send(401, "text/plain", "Unauthorized");

  DynamicJsonDocument doc(1024);
  for (const auto& dev : devices) {
    doc[dev.loadID] = dev.state ? 1 : 0;
  }
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

/**
 * @brief Updates device states based on received JSON.
 *        Body example: {"load1":1, "load2":0}
 */
void handleUpdateState() {
  if (!isAuthorizedAPI()) return server.send(401, "text/plain", "Unauthorized");

  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, body);
  if (err) return server.send(400, "text/plain", "Invalid JSON");

  JsonObject root = doc.as<JsonObject>();
  for (JsonPair kv : root) {
    String loadID = kv.key().c_str();
    int reqState = kv.value().as<int>();

    for (auto& dev : devices) {
      if (dev.loadID == loadID) {
        dev.state = (reqState == 1);
        applyHardwareState(dev);
        saveStatesToFile();
        updateFirebaseState(dev.loadID, dev.state);
        break;
      }
    }
  }
  handleGetState();  // return updated states
}

/**
 * @brief Deletes a device by its ID (query parameter ?id=...).
 */
void handleDeleteLoad() {
  if (!isAuthorizedAPI()) return server.send(401, "text/plain", "Unauthorized");
  if (!server.hasArg("id")) return server.send(400, "text/plain", "Missing load ID");

  String targetID = server.arg("id");
  auto it = std::find_if(devices.begin(), devices.end(),
                         [&](const Device& d) {
                           return d.loadID == targetID;
                         });
  if (it == devices.end()) {
    return server.send(404, "text/plain", "Load not found");
  }

  devices.erase(it);
  saveConfigToFile();
  saveStatesToFile();

  if (isFirebaseConnected) {
    String jsonPayload = "{\"config/" + targetID + "\":null, \"states/" + targetID + "\":null}";
    Database.update<object_t>(aClientPush, "/CypherNode", object_t(jsonPayload), pushCallback, "webDelAtomic");
  }

  server.send(200, "text/plain", "Deleted Successfully");
}

/**
 * @brief Saves the current automations to a file.
 * 
 */
void saveAutomationsToFile() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();

  for (auto& rule : activeRules) {
    JsonObject obj = arr.createNestedObject();
    obj["id"] = rule.id;
    obj["type"] = rule.type;
    obj["triggerAbove"] = rule.triggerAbove;
    obj["tempCondition"] = rule.tempCondition; //done
    obj["hysteresis"] = rule.hysteresis;
    obj["hour"] = rule.hour;
    obj["minute"] = rule.minute;
    obj["loadID"] = rule.loadID;
    obj["active"] = rule.active;
  }

  File file = LittleFS.open("/auto.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
  }
}

/**
 * @brief load the automations from a file.
 * 
 */
void loadAutomationsFromFile() {
  if (!LittleFS.exists("/auto.json")) return;

  File file = LittleFS.open("/auto.json", "r");
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (!error) {
    activeRules.clear();
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
      AutoRule rule;
      rule.id = obj["id"].as<String>();
      rule.type = obj["type"].as<String>();
      rule.loadID = obj["loadID"].as<String>();
      rule.active = obj["active"].as<bool>();

      if (rule.type == "temp") {
        rule.triggerAbove = obj["triggerAbove"].as<float>();
        rule.tempCondition = obj["tempCondition"].as<String>(); //done
        rule.hysteresis = obj["hysteresis"].as<float>();
        rule.hour = 0;
        rule.minute = 0;
      } else if (rule.type == "time") {
        rule.hour = obj["hour"].as<int>();
        rule.minute = obj["minute"].as<int>();
        rule.triggerAbove = 0.0;
      }
      activeRules.push_back(rule);
    }
  }
}

/**
* @brief updated listener
*/
void autoStreamCallback(AsyncResult& aResult) {
  if (aResult.isError() || !aResult.available()) return;
  RealtimeDatabaseResult& RTDB = aResult.to<RealtimeDatabaseResult>();

  if (RTDB.isStream()) {
    String eventType = RTDB.event();
    if (eventType != "put" && eventType != "patch") return; 

    String path = RTDB.dataPath();
    String valStr = RTDB.to<String>();

    // if deleted
    if (valStr == "null") {
      if (path == "/") { 
        // if all rules are deleted
        if (!activeRules.empty()) {
          activeRules.clear();
          saveAutomationsToFile();
          Serial.println("System: All Automations Cleared.");
        }
      } else {
        // when a single rule is deleted
        String ruleID = path.substring(1);
        auto it = std::remove_if(activeRules.begin(), activeRules.end(), [&](const AutoRule& r) { return r.id == ruleID; });
        if (it != activeRules.end()) {
          activeRules.erase(it, activeRules.end());
          saveAutomationsToFile();
          Serial.println("System: Rule " + ruleID + " Deleted.");
        }
      }
      return;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, valStr);
    if (err) return;

    JsonObject root = doc.as<JsonObject>();

    if (path == "/") {
      // when the entire database is updated together
      activeRules.clear();
      for (JsonPair kv : root) {
        AutoRule rule;
        rule.id = kv.key().c_str();
        JsonObject r = kv.value().as<JsonObject>();
        
        rule.type = r["type"].as<String>();
        rule.loadID = r["loadID"].as<String>();
        rule.active = r["active"].as<bool>();

        if (rule.type == "temp") {
          rule.triggerAbove = r["triggerAbove"].as<float>();
          rule.tempCondition = r.containsKey("tempCondition") ? r["tempCondition"].as<String>() : "above"; // done
          rule.hysteresis = r.containsKey("hysteresis") ? r["hysteresis"].as<float>() : 1.0;
          rule.hour = 0; rule.minute = 0;
        } else if (rule.type == "time") {
          rule.hour = r["hour"].as<int>();
          rule.minute = r["minute"].as<int>();
          rule.triggerAbove = 0.0;
        }
        activeRules.push_back(rule);
      }
      Serial.println("System: All Dynamic Rules Synced!");
      
    } else {
      // when a new rule is added or updated from the app
      String ruleID = path.substring(1); // remove / from path
      bool found = false;

      // if the rule already exists, it will be updated
      for (auto& rule : activeRules) {
        if (rule.id == ruleID) {
          if (root.containsKey("type")) rule.type = root["type"].as<String>();
          if (root.containsKey("loadID")) rule.loadID = root["loadID"].as<String>();
          if (root.containsKey("active")) rule.active = root["active"].as<bool>();
          if (rule.type == "temp" && root.containsKey("triggerAbove")) {
            rule.triggerAbove = root["triggerAbove"].as<float>();
            rule.tempCondition = root.containsKey("tempCondition") ? root["tempCondition"].as<String>() : "above"; //done
            rule.hysteresis = root.containsKey("hysteresis") ? root["hysteresis"].as<float>() : 1.0;
          }
          if (rule.type == "time") {
            if (root.containsKey("hour")) rule.hour = root["hour"].as<int>();
            if (root.containsKey("minute")) rule.minute = root["minute"].as<int>();
          }
          // reset lock flags for proper functioning
          rule.lastConditionMet = false;
          rule.lastTriggeredDay = -1;

          found = true;
          break;
        }
      }

      // if the rule does not exist, it will be added
      if (!found) {
        AutoRule rule;
        rule.id = ruleID;
        rule.type = root["type"].as<String>();
        rule.loadID = root["loadID"].as<String>();
        rule.active = root["active"].as<bool>();
        rule.actionTurnOn = root.containsKey("actionTurnOn") ? root["actionTurnOn"].as<bool>() : true;
        rule.lastConditionMet = false;
        rule.lastTriggeredDay = -1;
        if (rule.type == "temp") {
          rule.triggerAbove = root["triggerAbove"].as<float>();
          rule.tempCondition = root.containsKey("tempCondition") ? root["tempCondition"].as<String>() : "above"; //done
          rule.hysteresis = root.containsKey("hysteresis") ? root["hysteresis"].as<float>() : 1.0;
          rule.hour = 0; 
          rule.minute = 0;
        } else if (rule.type == "time") {
          rule.hour = root["hour"].as<int>();
          rule.minute = root["minute"].as<int>();
          rule.triggerAbove = 0.0;
        }
        activeRules.push_back(rule);
      }
      Serial.println("System: Rule " + ruleID + " Synced!");
    }
    
    saveAutomationsToFile();
  }
}

// ==========================================================
// Physical Switch Handling
// ==========================================================

/**
 * @brief Reads all physical switches with debouncing and updates device states accordingly.
 *        Called frequently from loop().
 */
void handlePhysicalSwitches() {
  unsigned long now = millis();

  for (auto& dev : devices) {
    if (!dev.hasPhysicalSwitch) continue;

    bool reading = digitalRead(dev.switchGPIO);

    // If reading changed, reset debounce timer
    if (reading != dev.lastSwitchState) {
      dev.lastDebounceTime = now;
    }

    // If enough time passed, consider the reading stable
    if ((now - dev.lastDebounceTime) > DEBOUNCE_DELAY) {
      if (reading != dev.stableSwitchState) {
        dev.stableSwitchState = reading;

        // For switches wired with INPUT_PULLUP, LOW means pressed (ON)
        bool switchIsOn = (reading == LOW);
        if (dev.state != switchIsOn) {
          dev.state = switchIsOn;
          applyHardwareState(dev);
          saveStatesToFile();
          updateFirebaseState(dev.loadID, dev.state);
        }
      }
    }
    dev.lastSwitchState = reading;
  }
}

// ==========================================================
// Initialize Cloud & Network Services (New Function)
// ==========================================================
void initCloudServices() {
  wifiManager.setServer(&server);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);  // for time

  // Setup Firebase
  sslStates.setInsecure();
  sslCmds.setInsecure();
  sslPush.setInsecure();
  sslAuto.setInsecure();  // automations

  initializeApp(aClientPush, app, getAuth(userAuth));
  app.getApp<RealtimeDatabase>(Database);
  Database.url(FIREBASE_DATABASE_URL);

  isFirebaseConnected = true;

  // Sync all data to Firebase on boot
  syncAllToFirebaseAtomic();

  // Start listening to command stream
  Database.get(aClientCmds, cmdsPath, cmdsStreamCallback, true, "streamCmds");
  Database.get(aClientAuto, "/CypherNode/autoRules", autoStreamCallback, true, "streamAuto");

  // Configure HTTP header collection for API key
  const char* headerKeys[] = { "x-api-key" };
  server.collectHeaders(headerKeys, 1);

  // Setup HTTP routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/get-config", HTTP_GET, handleGetConfig);
  server.on("/save-config", HTTP_POST, handleSaveConfig);
  server.on("/state", HTTP_GET, handleGetState);
  server.on("/update", HTTP_POST, handleUpdateState);
  server.on("/delete", HTTP_DELETE, handleDeleteLoad);

  server.begin();
  pushSystemLog("CypherNode server booted and connected.");
}

// ==========================================================
// Arduino Setup & Loop
// ==========================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  initSensors();

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    // Serial.println("LittleFS mount failed!");
    return;
  }

  // Load configuration (and states if available)
  loadConfigFromFile();
  loadAutomationsFromFile();

  // for non-blocking wifi logic
  wifiManager.begin();
  Serial.println("System: Initiation WiFi connection...");
  wifiManager.connectToWiFi(); // background wifi connection
  /*
  // Connect to WiFi (or start AP)
  if (!wifiManager.connectToWiFi()) {
    wifiManager.startAPMode(server);
  } else {
    wifiManager.setServer(&server);

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);  // for time

    // Setup Firebase
    sslStates.setInsecure();
    sslCmds.setInsecure();
    sslPush.setInsecure();
    sslAuto.setInsecure();  // automations

    initializeApp(aClientPush, app, getAuth(userAuth));
    app.getApp<RealtimeDatabase>(Database);
    Database.url(FIREBASE_DATABASE_URL);

    isFirebaseConnected = true;

    // Sync all data to Firebase on boot
    syncAllToFirebaseAtomic();

    // Start listening to command stream
    Database.get(aClientCmds, cmdsPath, cmdsStreamCallback, true, "streamCmds");
    Database.get(aClientAuto, "/CypherNode/autoRules", autoStreamCallback, true, "streamAuto");

    // (Optional) start listening to state stream if you want bidirectional cloud control
    // Database.get(aClientStates, basePath, statesStreamCallback, true, "streamStates");

    // Configure HTTP header collection for API key
    const char* headerKeys[] = { "x-api-key" };
    server.collectHeaders(headerKeys, 1);

    // Setup HTTP routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/get-config", HTTP_GET, handleGetConfig);
    server.on("/save-config", HTTP_POST, handleSaveConfig);
    server.on("/state", HTTP_GET, handleGetState);
    server.on("/update", HTTP_POST, handleUpdateState);
    server.on("/delete", HTTP_DELETE, handleDeleteLoad);

    server.begin();

    pushSystemLog("CypherNode server booted and connected.");
  }*/
}

void loop() {
  unsigned long currentMillis = millis();

  // ==========================================
  // CENTRALIZED SERIAL COMMAND ROUTER
  // ==========================================
  wifiManager.process();
  if (Serial.available()) {
    String serialData = Serial.readStringUntil('\n');
    serialData.trim();
    
    // if command start with "WIFI"
    if (serialData.startsWith("WIFI")) {
      String wifiCmd = serialData.substring(5); 
      wifiManager.executeCommand(wifiCmd, Serial);
    } 
    else if (serialData.startsWith("SYS ")) {
      String sysCmd = serialData.substring(4);
      if (sysCmd == "REBOOT" || sysCmd == "RESTART") {
        Serial.println("System: Rebooting...");
        delay(1000);
        ESP.restart();
      }
    }
    else {
      Serial.println("[Main] Unknown command: " + serialData);
    }
  }

  // State machine for init connection
  WiFiState currentState = wifiManager.getState();

  if (currentState == WIFI_STATE_CONNECTED && !connectionHandled) {
    Serial.println("System: WiFi Connected Succesfully!");
    initCloudServices();
    connectionHandled = true;
  } 
  else if (currentState == WIFI_SCAN_FAILED && !apModeStarted) {
    Serial.println("System: WiFi Failed. Starting AP Mode portal....");
    wifiManager.startAPMode(server);
    apModeStarted = true;
  }

  // Handle WebServer Clients (Only if AP or STA is running)
  if (connectionHandled || apModeStarted) {
    server.handleClient();
  }

  // ==========================================
  // 1. NETWORK HEALING (Non-Blocking)
  // ==========================================
  static bool wasWifiConnected = true;
  if (connectionHandled) {
    if (WiFi.status() != WL_CONNECTED) {
      if (wasWifiConnected) {
        Serial.println("System: WiFi Connection Lost!");
        wasWifiConnected = false;
        isFirebaseConnected = false;
      }
      if (currentMillis - lastWifiRetryTime > wifiRetryInterval) {
        lastWifiRetryTime = currentMillis;
        WiFi.disconnect();
        WiFi.reconnect();
      }
    } else {
      if (!wasWifiConnected) {
        Serial.println("System: WiFi Restored!");
        wasWifiConnected = true;
        isFirebaseConnected = true; 
      }
      // Firebase loop
      if (isFirebaseConnected) {
        app.loop();
        Database.loop();
      }
    }
  }

  // ==========================================
  // 2. PHYSICAL SWITCHES
  // ==========================================
  handlePhysicalSwitches();

  // ==========================================
  // 3. DELAYED FLASH WRITE (The Magic Trick)
  // ==========================================
  if (stateNeedsSave && (currentMillis - lastStateSaveTime > 3000)) {
    saveStatesToFile();
    stateNeedsSave = false;
  }

  // ==========================================
  // 4. FAST SENSOR POLLING (Every 2.5s)
  // ==========================================
  static float temp = NAN, hum = NAN, vol = NAN, cur = NAN;
  static unsigned long lastSensorReadTime = 0;
  if (currentMillis - lastSensorReadTime > 2500) {
    lastSensorReadTime = currentMillis;
    readSensorData(temp, hum, vol, cur);
  }

  // ==========================================
  // 5. EDGE AUTOMATION (Every 1.5s)
  // ==========================================
  static unsigned long lastAutoCheckTime = 0;
  if (currentMillis - lastAutoCheckTime > 1500) {
    lastAutoCheckTime = currentMillis;
    struct tm timeinfo;
    bool timeKnown = getLocalTime(&timeinfo);

    for (auto& rule : activeRules) {
      if (!rule.active) continue;

      for (auto& dev : devices) {
        if (dev.loadID == rule.loadID) {
          
          // --- Temperature Rule (With Above/Below Logic) ---
          // if (rule.type == "temp" && !isnan(temp) && temp > 0) {
          if (rule.type == "temp" && !isnan(temp)) {            
            // "Drops Below" (<) Logic
            if (rule.tempCondition == "below") {
              float resetThreshold = rule.triggerAbove + rule.hysteresis;
              
              if (temp <= rule.triggerAbove) {
                if (!rule.lastConditionMet) {
                  rule.lastConditionMet = true; 
                  if (dev.state != rule.actionTurnOn) {
                    dev.state = rule.actionTurnOn;
                    applyHardwareState(dev);
                    if (isFirebaseConnected) updateFirebaseState(dev.loadID, dev.state);
                    stateNeedsSave = true; lastStateSaveTime = currentMillis;
                    pushSystemLog("Temp Rule [" + rule.id + "] Executed (Below)");
                  }
                }
              } else if (temp >= resetThreshold) {
                if (rule.lastConditionMet) {
                  rule.lastConditionMet = false; // Unlock
                }
              }
            } 
            // "Goes Above" (>) Logic
            else {
              float resetThreshold = rule.triggerAbove - rule.hysteresis; 
              
              if (temp >= rule.triggerAbove) {
                if (!rule.lastConditionMet) {
                  rule.lastConditionMet = true; 
                  if (dev.state != rule.actionTurnOn) {
                    dev.state = rule.actionTurnOn;
                    applyHardwareState(dev);
                    if (isFirebaseConnected) updateFirebaseState(dev.loadID, dev.state);
                    stateNeedsSave = true; lastStateSaveTime = currentMillis;
                    pushSystemLog("Temp Rule [" + rule.id + "] Executed (Above)");
                  }
                }
              } else if (temp <= resetThreshold) {
                if (rule.lastConditionMet) {
                  rule.lastConditionMet = false; // Unlock
                }
              }
            }
          }
          // --- Time Rule ---
          else if (rule.type == "time" && timeKnown) {
            // if (timeinfo.tm_hour == rule.hour && timeinfo.tm_min == rule.minute && timeinfo.tm_sec < 5) {
            if (timeinfo.tm_hour == rule.hour && timeinfo.tm_min == rule.minute) {
              if (rule.lastTriggeredDay != timeinfo.tm_mday) {
                rule.lastTriggeredDay = timeinfo.tm_mday;
                if (dev.state != rule.actionTurnOn) {
                  dev.state = rule.actionTurnOn;
                  applyHardwareState(dev);
                  if (isFirebaseConnected) updateFirebaseState(dev.loadID, dev.state);
                  
                  stateNeedsSave = true;
                  lastStateSaveTime = currentMillis;
                  pushSystemLog("Time Rule [" + rule.id + "] Executed");
                }
              }
            }
          }
          break; 
        }
      }
    }
  }

  // ==========================================
  // 6. FIREBASE ALERTS & SYNC (Every 20s)
  // ==========================================
  if (currentMillis - lastHeartbeatTime > 20000) {
    lastHeartbeatTime = currentMillis;

    if (isFirebaseConnected) {
      DynamicJsonDocument doc(512);
      JsonObject healthObj = doc.createNestedObject("health");
      healthObj["lastPulse"][".sv"] = "timestamp";

#ifdef ENABLE_DHT
      JsonObject dhtObj = doc.createNestedObject("sensors/dht");
      dhtObj["temp"] = temp;
      dhtObj["humidity"] = hum;
      
      if (!isnan(temp)) {
        if (temp >= 40.0 && !highTempAlertSent) {
          DynamicJsonDocument alertDoc(256);
          alertDoc['id'] = "sys_alert_" + String(millis());
          alertDoc["type"] = "warning";
          alertDoc["title"] = "High Temperature Alert!";
          alertDoc["body"] = "Current room temperature is " + String(temp, 1) + "°C.";
          alertDoc["value"] = temp;
          alertDoc["timestamp"] [".sv"] = "timestamp";
          alertDoc["read"] = false;

          String alertPayload;
          serializeJson(alertDoc, alertPayload);
          Database.push<object_t>(aClientPush, temp_alert, object_t(alertPayload), pushCallback, "alertTask");
          highTempAlertSent = true;
          Serial.println("System: High Temp Alert pushed");
        } else if (temp < 38.0 && highTempAlertSent) {
          highTempAlertSent = false;
          Database.remove(aClientPush, temp_alert, pushCallback, "clearAlertTask");
          Serial.println("System: High Temp Alert cleared");
        }
      }
#endif

#ifdef ENABLE_VAC
      JsonObject vacObj = doc.createNestedObject("sensors/vac");
      vacObj["voltage"] = vol;
      vacObj["current"] = cur;
#endif

      String payload;
      serializeJson(doc, payload);
      Database.update<object_t>(aClientPush, "/CypherNode", object_t(payload), pushCallback, "sensorPulseTask");
    }
  }
}