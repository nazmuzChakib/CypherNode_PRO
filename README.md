# CypherNode - Enterprise-Grade IoT Smart Home Automation

![CypherNode Banner](https://via.placeholder.com/1200x400.png?text=CypherNode+by+Cypher-Z) 
*(You can replace this link with a real banner image of your app/hardware)*

CypherNode is an advanced, highly secure, and highly optimized Smart Home Automation system developed by **Team Cypher-Z**. Unlike traditional direct-state-mutation IoT projects, CypherNode utilizes an enterprise-level **Command-Queue Architecture**, ensuring zero data conflicts, strict hardware acknowledgments, and absolute system reliability.

---

## 🚀 Key Features

* **Command-Queue Architecture:** The mobile app does not directly alter hardware states. Instead, it pushes commands to a dedicated Firebase queue. The ESP32 server processes the command, applies hardware changes, reports the real state back, and strictly clears the queue (Acknowledgment).
* **Role-Based Access Control (RBAC):** Strict security rules are implemented at the Firebase RTDB level. Only designated "Admins" can initialize new nodes, delete devices, or send system commands (like remote reboots). Regular users can only toggle authorized loads.
* **Offline Failsafe & Ghost State Prevention:** * **App Side:** Monitors Firebase `.info/connected` and Server Heartbeats. If the ESP32 drops offline, the app dynamically disables UI controls to prevent offline state mutations.
    * **Server Side:** Uses a non-blocking debounce algorithm to track physical switch stable states, preventing "Ghost Overrides" when syncing with the cloud.
* **Dynamic GPIO Allocation:** When initializing a new load via the Admin Dashboard, the system dynamically scans the current configuration and only provides available, safe GPIO pins, preventing short-circuits or hardware conflicts.
* **Atomic Data Synchronization:** System configurations, hardware states, and command executions are grouped into single atomic JSON payloads to save bandwidth and prevent partial data corruption.
* **OTA ABI-based App Updates:** The Flutter application integrates seamlessly with the GitHub Releases API to fetch architecture-specific APKs (`arm64-v8a`, `armeabi-v7a`), ensuring minimal app size and optimal performance.

---

## 🏗️ System Architecture

The project is divided into two main ecosystems:

### 1. Hardware Server (ESP32)
* **Core:** Written in C++ utilizing asynchronous Firebase operations (`FirebaseClient`).
* **Storage:** `LittleFS` is used for local persistence of configurations and states to survive power outages.
* **Network:** Utilizes `ESPWiFiManager` for dynamic network provisioning (AP mode fallback).
* **Safety:** Implements Active-High/Active-Low relay support dynamically via configuration.

### 2. Client Application (Flutter)
* **Framework:** Built with Flutter for cross-platform support.
* **State Management:** Uses `Provider` for dynamic theming (Dark Mode/Accent Colors) and Authentication states.
* **Optimistic UI:** Implements a "Pending State" UI with timeout failsafes while waiting for ESP32 acknowledgments, ensuring a buttery-smooth user experience.

---

## 🗄️ Database Structure (Firebase RTDB)

The system relies on a strictly governed Realtime Database structure:

```json
{
  "CypherNode": {
    "config": { "L1": { "loadGPIO": 2, "hasPhysicalSwitch": true ... } },
    "states": { "L1": 1, "F1": 0 },
    "commands": {
      "toggle": { "L1": 1 },
      "add": { ... },
      "delete": { "L1": true },
      "system": { "reboot": true }
    },
    "health": { "pulse": 1420 },
    "logs": { "-NxYz...": { "message": "Remote reboot initiated.", "timestamp": 1710200000 } }
  },
  "users": {
    "uid_123": { "role": "admin", "email": "admin@cypher.z" }
  }
}```