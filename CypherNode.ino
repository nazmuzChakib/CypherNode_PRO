/**
 * @file CypherNode.ino
 * @author Team Cypher-Z
 * @brief Optimized Dynamic Smart Home Server with Active-Low Support & Command Queue. Update functionality
 * @version 3.0.1 (new architecture)
 * @date 2026-03-12
 */

// Enable Sensors
#define ENABLE_DHT
// #define ENABLE_VAC // when use voltage sensor then uncomment it

#include "Sensors.h"

#include <time.h>  // for time

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESPWiFiManager.h>
#include <LittleFS.h>
#include <vector>


// for mac
#include <esp_mac.h>

#include "WebPage.h"
#include "secrets.h"  // ⚠️ Private credentials — excluded from git

#define ENABLE_DATABASE
#define ENABLE_USER_AUTH

#include <FirebaseClient.h>
#include <WiFiClientSecure.h>

// ==========================================================
// HTTP Authentication Credentials (loaded from secrets.h)
// ==========================================================
const char* HTTP_USERNAME = HTTP_USERNAME_SECRET;
const char* HTTP_PASSWORD = HTTP_PASSWORD_SECRET;
const String API_KEY = API_KEY_SECRET;

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
bool initialSyncDone = false;

// wifimanager lib flag
bool apModeStarted = false;
bool connectionHandled = false;

// ==========================================================
// Dynamic Node Identification & Firebase Paths
// ==========================================================
String NODE_ID;

String nodeRootPath;  // Main folder of this node in Firebase
String basePath;
String configPath;
String cmdsPath;
String logsPath;
String togglePath;
String healthPath;
String temp_alert;
String autoPath;  // For automation rules
String ipPath;    // For saving local IP

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
/**
 * @struct Device
 * @brief Represents a physical device (load) controlled by the node.
 */
struct Device {
  String loadID;                       ///< Unique identifier for the device (e.g., "fan_1")
  String loadName;                     ///< Human-readable name (e.g., "Ceiling Fan")
  String loadType;                     ///< Type of load (e.g., "light", "fan")
  int loadGPIO;                        ///< Output GPIO pin number
  bool hasPhysicalSwitch;              ///< Whether the device has a physical toggle switch
  int switchGPIO;                      ///< Input GPIO pin for the physical switch
  bool activeHigh;                     ///< Polarity: true = HIGH turns ON, false = LOW turns ON
  bool state;                          ///< Current logical state (true = ON, false = OFF)
  bool lastSwitchState;                ///< Last raw reading from the physical switch
  unsigned long lastDebounceTime = 0;  ///< Timestamp of last state change for debouncing
  bool stableSwitchState;              ///< Debounced stable switch state
};

// ==========================================================
// Automation rule list
// ==========================================================
/**
 * @struct AutoRule
 * @brief Represents an automation rule (temperature or time based).
 */
struct AutoRule {
  String id;                       ///< Unique ID of the rule
  String type;                     ///< Type: "temp" or "time"
  float triggerAbove;              ///< Trigger value for temperature
  String tempCondition = "above";  ///< "above" or "below"
  float hysteresis = 1.0;          ///< Tolerance to prevent rapid toggling
  int hour;                        ///< Trigger hour (for time-based rules)
  int minute;                      ///< Trigger minute (for time-based rules)
  String loadID;                   ///< ID of the target device to control
  bool active;                     ///< Whether the rule is currently enabled

  // New variables for distributed control
  String targetNodeID;  ///< Node ID of the device to control (if remote)
  String targetIP;      ///< Local IP of the target node (if remote)

  bool actionTurnOn;      ///< Action to perform: true = ON, false = OFF
  bool lastConditionMet;  ///< Lock flag to prevent repeated triggers
  int lastTriggeredDay;   ///< Prevents time rules from firing more than once per day
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
void handlePing();


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

  Database.update<object_t>(aClientPush, nodeRootPath, object_t(jsonStr), pushCallback, "atomicSyncAll");
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
      // saveStatesToFile();
      stateNeedsSave = true;
      lastStateSaveTime = millis();

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
  Database.update<object_t>(aClientPush, nodeRootPath, object_t(jsonPayload), pushCallback, "appDelAtomic");

  pushSystemLog("Device " + targetID + " removed.");
  // Serial.println("✓ Executed delete for: " + targetID);
}

/**
 * @brief Processes an add command with Edge-Level Validation.
 * @param targetID New device ID.
 * @param valStr   JSON string containing device configuration.
 */
void processAddCommand(const String& targetID, const String& valStr) {
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, valStr);
  if (error) {
    Database.remove(aClientPush, cmdsPath + "/add/" + targetID, pushCallback, "ackAddErr");
    return;
  }

  int newLoadGPIO = doc["loadGPIO"].as<int>();
  bool newHasSwitch = doc["hasPhysicalSwitch"].as<bool>();
  int newSwitchGPIO = newHasSwitch ? doc["switchGPIO"].as<int>() : -1;

  // ==========================================
  // EDGE LEVEL VALIDATOR (Duplicate Check Logic)
  // ==========================================
  bool isDuplicate = false;
  String errorMsg = "";

  // SELF COLLISION CHECK (New!)
  if (newHasSwitch && newLoadGPIO == newLoadGPIO) {
    pushSystemLog("Validation Failed for " + targetID + ": Load GPIO and Switch GPIO cannot be the same");
    Database.remove(aClientPush, cmdsPath + "/add/" + targetID, pushCallback, "ackAddErr");
    return;
  }

  for (const auto& d : devices) {
    // 1. Load ID duplicate check
    if (d.loadID == targetID) {
      isDuplicate = true;
      errorMsg = "Duplicate Load ID";
      break;
    }
    // 2. Load GPIO duplicate check
    if (d.loadGPIO == newLoadGPIO) {
      isDuplicate = true;
      errorMsg = "Load GPIO " + String(newLoadGPIO) + " is already in use";
      break;
    }
    // 3. Switch GPIO duplicate check
    if (d.hasPhysicalSwitch && newHasSwitch && d.switchGPIO == newSwitchGPIO) {
      isDuplicate = true;
      errorMsg = "Switch GPIO " + String(newSwitchGPIO) + " is already in use";
      break;
    }
    // 4. Cross-Check: check new load pin already in use as a switch?
    if (d.hasPhysicalSwitch && d.switchGPIO == newLoadGPIO) {
      isDuplicate = true;
      errorMsg = "GPIO " + String(newLoadGPIO) + " is already used as a Switch";
      break;
    }
    // ৫. Cross-Check: check new switch pin already in use as a load?
    if (newHasSwitch && d.loadGPIO == newSwitchGPIO) {
      isDuplicate = true;
      errorMsg = "GPIO " + String(newSwitchGPIO) + " is already used as a Load";
      break;
    }
  }

  if (isDuplicate) {
    // if validation fails, push log and remove command
    pushSystemLog("Validation Failed for " + targetID + ": " + errorMsg);
    Serial.println("System: " + errorMsg);
    Database.remove(aClientPush, cmdsPath + "/add/" + targetID, pushCallback, "ackAddErr");
    return;
  }
  // ==========================================

  // add new device after passing validation
  Device dev;
  dev.loadID = targetID;
  dev.loadName = doc["loadName"].as<String>();
  dev.loadType = doc["loadType"].as<String>();
  dev.loadGPIO = newLoadGPIO;
  dev.hasPhysicalSwitch = newHasSwitch;
  dev.switchGPIO = newSwitchGPIO;
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
  Database.update<object_t>(aClientPush, nodeRootPath, object_t(jsonPayload), pushCallback, "addAtomic");

  pushSystemLog("New device " + targetID + " added successfully.");

  // Remove command from queue
  Database.remove(aClientPush, cmdsPath + "/add/" + targetID, pushCallback, "ackAdd");
}

/**
 * @brief Processes a system command (e.g., reboot).
 * @param command Command name.
 * @param valStr  Value (truthy to execute).
 */
void processSystemCommand(const String& command, const String& valStr) {
  bool execute = (valStr == "true" || valStr.toInt() == 1);
  if (!execute) return;

  Database.remove(aClientPush, cmdsPath + "/system/" + command, pushCallback, "ackSystem");

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
  // 1. CORS Preflight Bypass (importent for Cross-Node UI)
  if (server.method() == HTTP_OPTIONS) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS, DELETE");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, x-api-key, Authorization");
    server.send(204);  // No Content response for preflight
    return true;       // Let the options request pass without auth
  }

  // 2. Add CORS headers to actual responses so browser can read them
  server.sendHeader("Access-Control-Allow-Origin", "*");

  // 3. Normal Authentication Check
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

  String payload = server.arg("plain");

  // check first if data is valid or not
  DynamicJsonDocument testDoc(2048);
  DeserializationError err = deserializeJson(testDoc, payload);
  if (err) {
    return server.send(400, "text/plain", "Invalid JSON");
  }

  // Write new config file
  File file = LittleFS.open("/config.json", "w");
  file.print(payload);
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
 * @brief handle ping
 * @return void
 */
void handlePing() {
  server.send(200, "text/plain", "OK");
}

// ==========================================================
// Cross-Node HTTP Engine (Local Network Communication)
// ==========================================================

/**
 * @brief Sends an HTTP POST request to another ESP32 node to update a device state.
 * @param targetIP  The local IP address of the target ESP32 (e.g., "192.168.1.104").
 * @param loadID    The ID of the device to control (e.g., "fan_1").
 * @param state     The desired state (1 for ON, 0 for OFF).
 */
// void sendLocalCrossNodeCommand(String targetIP, String loadID, int state) {
//   // Send request only if WiFi is connected
//   if (WiFi.status() == WL_CONNECTED) {
//     HTTPClient http;

//     // Construct /update endpoint for target ESP32
//     String url = "http://" + targetIP + "/update";
//     http.begin(url);

//     // Set Content-Type and API_KEY in header for security
//     http.addHeader("Content-Type", "application/json");
//     http.addHeader("x-api-key", API_KEY);

//     // Create JSON payload (e.g., {"fan_1": 1})
//     String jsonPayload = "{\"" + loadID + "\":" + String(state) + "}";

//     // Send POST request
//     int httpResponseCode = http.POST(jsonPayload);

//     if (httpResponseCode > 0) {
//       Serial.println("System: Cross-node command sent to " + targetIP + " for " + loadID + ". HTTP Code: " + String(httpResponseCode));
//     } else {
//       Serial.println("Error: Cross-node command failed. HTTP Error: " + String(httpResponseCode));
//     }

//     // Release memory
//     http.end();
//   } else {
//     Serial.println("Error: WiFi disconnected, cannot send cross-node command.");
//   }
// }

// ==========================================================
// Cross-Node HTTP Engine (Smart Hybrid Routing)
// ==========================================================
/**
 * @brief Sends command to another node locally using mDNS (No Static IP required).
 */
void sendLocalCrossNodeCommand(String targetNodeID, String targetIP, String loadID, int state) {
  bool localSuccess = false;

  if (WiFi.status() == WL_CONNECTED && targetNodeID != "") {
    HTTPClient http;
    http.setTimeout(3000);

    // Using mDNS local domain instead of IP
    String targetHost = "cypher-" + targetNodeID + ".local";
    String url = "http://" + targetHost + "/update";

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", API_KEY);

    String jsonPayload = "{\"" + loadID + "\":" + String(state) + "}";
    int httpResponseCode = http.POST(jsonPayload);

    // 200 OK means mDNS worked and command executed
    if (httpResponseCode == 200) {
      localSuccess = true;
      Serial.println("System: ✅ Cross-node command Executed LOCALLY on " + targetHost);
    } else {
      Serial.println("Error: ⚠️ Cross-node mDNS failed (Code: " + String(httpResponseCode) + "). Trying cached IP...");

      // FALLBACK: If router doesn't support mDNS, try cached IP
      if (targetIP != "") {
        http.end();
        String fallbackUrl = "http://" + targetIP + "/update";
        http.begin(fallbackUrl);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("x-api-key", API_KEY);
        int fallbackResponse = http.POST(jsonPayload);

        if (fallbackResponse == 200) {
          localSuccess = true;
          Serial.println("System: ✅ Cross-node command Executed on Cached IP: " + targetIP);
        }
      }
    }
    http.end();
  }

  // 3. FIREBASE FALLBACK (if Local mDNS and IP both fail)
  if (!localSuccess && isFirebaseConnected && targetNodeID != "") {
    Serial.println("System: ☁️ Local cross-node completely failed. Pushing to Firebase Cloud...");
    String targetFirebasePath = "/CypherNode/nodes/" + targetNodeID + "/states/" + loadID;
    Database.set<int>(aClientPush, targetFirebasePath, state, pushCallback, "crossNodeFallback");
  }
}

/**
 * @brief Updates device states based on received JSON from App.
 */
void handleUpdateState() {
  Serial.println("\n========== [LOCAL HTTP CONTROL] ==========");
  Serial.println("System: /update endpoint hit by App.");

  // 1. API Key verificaiton check
  if (!isAuthorizedAPI()) {
    Serial.println("Error: ❌ Unauthorized request! API Key mismatch.");
    return server.send(401, "text/plain", "Unauthorized");
  }
  Serial.println("System: ✅ API Key verified.");

  // 2. Read body (For Application/JSON)
  String body = "";
  if (server.hasArg("plain")) {
    body = server.arg("plain");
  } else if (server.args() > 0) {
    body = server.arg(0);
  }

  Serial.println("Payload Received: " + body);

  // 3. Check if body is empty
  if (body == "") {
    Serial.println("Error: ❌ Empty body received. App is not sending JSON properly.");
    return server.send(400, "text/plain", "Empty Body");
  }

  // 4. JSON Parse Check
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.println("Error: ❌ JSON Parse Failed: " + String(err.c_str()));
    return server.send(400, "text/plain", "Invalid JSON");
  }

  // 5. Find device and trigger relay
  JsonObject root = doc.as<JsonObject>();
  bool deviceFound = false;

  for (JsonPair kv : root) {
    String loadID = kv.key().c_str();
    int reqState = kv.value().as<int>();

    Serial.println("Target Device: [" + loadID + "] -> Action: " + (reqState == 1 ? "ON" : "OFF"));

    for (auto& dev : devices) {
      if (dev.loadID == loadID) {
        dev.state = (reqState == 1);
        applyHardwareState(dev);  // Relay ON/OFF
        // saveStatesToFile();
        stateNeedsSave = true;
        lastStateSaveTime = millis();

        if (isFirebaseConnected) {
          updateFirebaseState(dev.loadID, dev.state);
        }
        deviceFound = true;
        Serial.println("Success: ✅ Relay switched LOCALLY.");
        break;
      }
    }
  }

  if (!deviceFound) {
    Serial.println("Warning: ⚠️ Device ID not found on this node.");
    return server.send(404, "text/plain", "Device Not Found");
  }

  Serial.println("==========================================\n");
  handleGetState();  // Response will be 200 OK and current status
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
    Database.update<object_t>(aClientPush, nodeRootPath, object_t(jsonPayload), pushCallback, "webDelAtomic");
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
    obj["tempCondition"] = rule.tempCondition;  //done
    obj["hysteresis"] = rule.hysteresis;
    obj["hour"] = rule.hour;
    obj["minute"] = rule.minute;
    obj["loadID"] = rule.loadID;
    obj["targetNodeID"] = rule.targetNodeID;
    obj["targetIP"] = rule.targetIP;
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
      // If target node ID is missing, default to own NODE_ID
      rule.targetNodeID = obj.containsKey("targetNodeID") ? obj["targetNodeID"].as<String>() : NODE_ID;
      rule.targetIP = obj.containsKey("targetIP") ? obj["targetIP"].as<String>() : "";
      rule.active = obj["active"].as<bool>();

      if (rule.type == "temp") {
        rule.triggerAbove = obj["triggerAbove"].as<float>();
        rule.tempCondition = obj["tempCondition"].as<String>();  //done
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
        auto it = std::remove_if(activeRules.begin(), activeRules.end(), [&](const AutoRule& r) {
          return r.id == ruleID;
        });
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
        // If target node ID is missing, default to own NODE_ID
        rule.targetNodeID = r.containsKey("targetNodeID") ? r["targetNodeID"].as<String>() : NODE_ID;
        rule.targetIP = r.containsKey("targetIP") ? r["targetIP"].as<String>() : "";
        rule.active = r["active"].as<bool>();

        if (rule.type == "temp") {
          rule.triggerAbove = r["triggerAbove"].as<float>();
          rule.tempCondition = r.containsKey("tempCondition") ? r["tempCondition"].as<String>() : "above";  // done
          rule.hysteresis = r.containsKey("hysteresis") ? r["hysteresis"].as<float>() : 1.0;
          rule.hour = 0;
          rule.minute = 0;
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
      String ruleID = path.substring(1);  // remove / from path
      bool found = false;

      // if the rule already exists, it will be updated
      for (auto& rule : activeRules) {
        if (rule.id == ruleID) {
          if (root.containsKey("type")) rule.type = root["type"].as<String>();
          if (root.containsKey("loadID")) rule.loadID = root["loadID"].as<String>();
          if (root.containsKey("active")) rule.active = root["active"].as<bool>();
          if (rule.type == "temp" && root.containsKey("triggerAbove")) {
            rule.triggerAbove = root["triggerAbove"].as<float>();
            rule.tempCondition = root.containsKey("tempCondition") ? root["tempCondition"].as<String>() : "above";  //done
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
          rule.tempCondition = root.containsKey("tempCondition") ? root["tempCondition"].as<String>() : "above";  //done
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
          // saveStatesToFile();
          // **DEBUG**
          stateNeedsSave = true;
          lastStateSaveTime = millis();
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
/**
 * @brief Initializes Cloud and Network services.
 *        Sets up WiFiManager, NTP, Firebase, and HTTP Server routes.
 */
void initCloudServices() {
  wifiManager.setServer(&server);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);  // for time

  String localHostname = "cypher-" + NODE_ID;
  if (MDNS.begin(localHostname.c_str())) {
    Serial.println("System: mDNS responder started at http://" + localHostname + ".local");
    MDNS.addService("http", "tcp", 80);
  }

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
  // syncAllToFirebaseAtomic();

  // Start listening to command stream
  Database.get(aClientCmds, cmdsPath, cmdsStreamCallback, true, "streamCmds");
  Database.get(aClientAuto, autoPath, autoStreamCallback, true, "streamAuto");

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
  server.on("/ping", HTTP_GET, handlePing);

  server.begin();
  pushSystemLog("CypherNode server booted and connected.");

  // ==========================================
  // Local Network Presence (Initial Sync)
  // ==========================================
  if (isFirebaseConnected) {
    String currentIP = WiFi.localIP().toString();

    String infoPayload = "{\"info/ip_address\": \"" + currentIP + "\"}";

    // Push info to info folder under nodeRootPath
    Database.update<object_t>(aClientPush, nodeRootPath, object_t(infoPayload), pushCallback, "infoInit");

    Serial.println("System: Initial Network Presence Synced. IP: " + currentIP);
  }
}

// ==========================================================
// Arduino Setup & Loop
// ==========================================================

/**
 * @brief Initial system setup.
 *        Configures Serial, reads MAC address, initializes sensors, LittleFS, and connects to WiFi.
 */
void setup() {
  Serial.begin(115200);
  delay(200);

  // 1. Read MAC Address directly from ESP32 eFuse without turning on WiFi radio
  uint8_t baseMac[6];
  esp_read_mac(baseMac, ESP_MAC_WIFI_STA);

  // Convert MAC Address to string (e.g., A1B2C3D4E5F6)
  char macStr[13];
  sprintf(macStr, "%02X%02X%02X%02X%02X%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  NODE_ID = String(macStr);

  // 2. Initialize dynamic Firebase paths
  nodeRootPath = "/CypherNode/nodes/" + NODE_ID;
  basePath = nodeRootPath + "/states";
  configPath = nodeRootPath + "/config";
  cmdsPath = nodeRootPath + "/commands";
  logsPath = nodeRootPath + "/logs";
  togglePath = nodeRootPath + "/commands/toggle/";
  healthPath = nodeRootPath + "/health/pulse";
  temp_alert = nodeRootPath + "/alerts/high_temp";
  autoPath = nodeRootPath + "/autoRules";
  ipPath = nodeRootPath + "/ip_address";

  Serial.println("\nSystem: Node Initialized. NODE_ID: " + NODE_ID);

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
  wifiManager.connectToWiFi();  // background wifi connection
}

/**
 * @brief Main execution loop.
 *        Handles WiFi, WebServer clients, physical switches, sensor polling, and automation rules.
 */
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
    if (serialData.startsWith("WIFI ")) {
      String wifiCmd = serialData.substring(5);
      wifiManager.executeCommand(wifiCmd, Serial);
    } else if (serialData.startsWith("SYS ")) {
      String sysCmd = serialData.substring(4);
      if (sysCmd == "REBOOT" || sysCmd == "RESTART") {
        Serial.println("System: Rebooting...");
        delay(1000);
        ESP.restart();
      }
    }  // Wairning: This process will delete all the data from the device. So use carefully and don't available on help command
    else if (serialData == "system_clear_fs_force") {
      Serial.println("System: Clearing LittleFS data...");
      File root = LittleFS.open("/");
      if (root) {
        File file = root.openNextFile();
        while (file) {
          String fileName = file.name();
          String fullPath = fileName.startsWith("/") ? fileName : "/" + fileName;
          // Protect any WiFi related files (most ESP32 libs use NVS, but this adds safety)
          if (fullPath.indexOf("wifi") == -1 && fullPath.indexOf("wm_") == -1) {
            LittleFS.remove(fullPath);
            Serial.println("Deleted: " + fullPath);
          }
          file = root.openNextFile();
        }
      }
      Serial.println("System: LittleFS Cleared! Restarting...");
      delay(1000);
      ESP.restart();
    } else {
      Serial.println("[Main] Unknown command: " + serialData);
    }
  }

  // State machine for init connection
  WiFiState currentState = wifiManager.getState();

  if (currentState == WIFI_STATE_CONNECTED && !connectionHandled) {
    Serial.println("System: WiFi Connected Succesfully!");
    initCloudServices();
    connectionHandled = true;
  } else if (currentState == WIFI_STATE_FAILED && !apModeStarted) {
    Serial.println("System: WiFi Failed. Starting AP Mode portal....");
    wifiManager.startAPMode(server);
    apModeStarted = true;
  }

  // ==========================================
  // 1. HANDLE WEB SERVER CLIENTS (PRIORITY: FIRST!)
  // ==========================================
  // IMPORTANT: handleClient() must run BEFORE app.loop() and Database.loop().
  // When internet is down, Firebase SDK internals can stall app.loop(), which
  // would delay HTTP responses and cause Flutter's local /ping to timeout,
  // incorrectly marking the node as offline even on a local-only network.
  if (connectionHandled || apModeStarted) {
    server.handleClient();
  }

  // ==========================================
  // 2. NETWORK HEALING & FIREBASE LOOP
  // ==========================================
  static bool wasWifiConnected = true;
  if (connectionHandled) {
    if (WiFi.status() != WL_CONNECTED) {
      // WiFi physically lost (e.g. router power off)
      if (wasWifiConnected) {
        Serial.println("System: WiFi Connection Lost!");
        wasWifiConnected = false;
        isFirebaseConnected = false;
        initialSyncDone = false;  // Allow re-sync when WiFi restores
      }
      if (currentMillis - lastWifiRetryTime > wifiRetryInterval) {
        lastWifiRetryTime = currentMillis;
        WiFi.disconnect();
        WiFi.reconnect();
      }
    } else {
      // WiFi is physically connected (local network OK, internet may or may not be available)
      if (!wasWifiConnected) {
        Serial.println("System: WiFi Restored!");
        wasWifiConnected = true;
        isFirebaseConnected = true;
      }

      // Always run Firebase SDK loops so it can self-recover when internet returns.
      // These calls are non-blocking by design when internet is unavailable —
      // the SDK queues retries internally without stalling the main loop.
      app.loop();
      Database.loop();

      // Initial Firebase sync once auth is ready
      if (isFirebaseConnected && app.ready() && !initialSyncDone) {
        syncAllToFirebaseAtomic();
        initialSyncDone = true;
        Serial.println("System: Firebase Auth Ready. Initial Firebase Synced!");
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
  // 5. DISTRIBUTED EDGE AUTOMATION (Every 1.5s)
  // ==========================================
  static unsigned long lastAutoCheckTime = 0;
  if (currentMillis - lastAutoCheckTime > 1500) {
    lastAutoCheckTime = currentMillis;
    struct tm timeinfo;
    bool timeKnown = getLocalTime(&timeinfo);

    for (auto& rule : activeRules) {
      if (!rule.active) continue;

      bool triggerAction = false;
      bool resetLock = false;

      // --- Condition Checking: Temperature Rule ---
      if (rule.type == "temp" && !isnan(temp)) {
        if (rule.tempCondition == "below") {
          float resetThreshold = rule.triggerAbove + rule.hysteresis;
          if (temp <= rule.triggerAbove && !rule.lastConditionMet) {
            triggerAction = true;
          } else if (temp >= resetThreshold && rule.lastConditionMet) {
            resetLock = true;
          }
        } else {  // "above" logic
          float resetThreshold = rule.triggerAbove - rule.hysteresis;
          if (temp >= rule.triggerAbove && !rule.lastConditionMet) {
            triggerAction = true;
          } else if (temp <= resetThreshold && rule.lastConditionMet) {
            resetLock = true;
          }
        }
      }
      // --- Condition Checking: Time Rule ---
      else if (rule.type == "time" && timeKnown) {
        if (timeinfo.tm_hour == rule.hour && timeinfo.tm_min == rule.minute) {
          if (rule.lastTriggeredDay != timeinfo.tm_mday) {
            triggerAction = true;
            rule.lastTriggeredDay = timeinfo.tm_mday;
          }
        }
      }

      // --- Action Execution (Local vs Remote) ---
      if (triggerAction) {
        rule.lastConditionMet = true;  // Lock the rule

        // Case 1: Is the target device local?
        if (rule.targetNodeID == "" || rule.targetNodeID == NODE_ID) {
          for (auto& dev : devices) {
            if (dev.loadID == rule.loadID && dev.state != rule.actionTurnOn) {
              dev.state = rule.actionTurnOn;
              applyHardwareState(dev);
              if (isFirebaseConnected) updateFirebaseState(dev.loadID, dev.state);
              stateNeedsSave = true;
              lastStateSaveTime = currentMillis;
              pushSystemLog("Local Rule Executed: " + rule.id);
              break;
            }
          }
        }
        // Case 2: Is the target device on another node? (Cross-Node Command)
        else {
          sendLocalCrossNodeCommand(rule.targetNodeID, rule.targetIP, rule.loadID, rule.actionTurnOn ? 1 : 0);
          pushSystemLog("Remote Rule Executed: " + rule.id + " -> " + rule.targetNodeID);
        }
      }

      // Unlock automation (For Hysteresis)
      if (resetLock) {
        rule.lastConditionMet = false;
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

      // --- UPDATED: Deep Path Update (No Nested Objects for Root) ---
      doc["info/ip_address"] = WiFi.localIP().toString();

      // for Firebase Server Timestamp
      JsonObject pulseObj = doc.createNestedObject("health/lastPulse");
      pulseObj[".sv"] = "timestamp";

#ifdef ENABLE_DHT
      // Deep Path for DHT
      if (isnan(temp) || isnan(hum)) {
        doc["sensors/dht/temp"] = nullptr;
        doc["sensors/dht/humidity"] = nullptr;
      } else {
        doc["sensors/dht/temp"] = temp;
        doc["sensors/dht/humidity"] = hum;
      }

      if (!isnan(temp)) {
        if (temp >= 40.0 && !highTempAlertSent) {
          DynamicJsonDocument alertDoc(256);
          alertDoc["id"] = "sys_alert_" + String(millis());
          alertDoc["type"] = "warning";
          alertDoc["title"] = "High Temperature Alert!";
          alertDoc["body"] = "Current room temperature is " + String(temp, 1) + "°C.";
          alertDoc["value"] = temp;
          alertDoc["timestamp"][".sv"] = "timestamp";
          alertDoc["read"] = false;

          String alertPayload;
          serializeJson(alertDoc, alertPayload);
          // Note: temp_alert path should be previously defined
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
      // Deep Path for VAC
      if (isnan(vol) || isnan(cur)) {
        doc["sensors/vac/voltage"] = nullptr;
        doc["sensors/vac/current"] = nullptr;
      } else {
        doc["sensors/vac/voltage"] = vol;
        doc["sensors/vac/current"] = cur;
      }
#endif

      String payload;
      serializeJson(doc, payload);

      Serial.println("Pulse Payload: " + payload);  // for debug

      Database.update<object_t>(aClientPush, nodeRootPath, object_t(payload), pushCallback, "sensorPulseTask");
    }
  }
}