# CypherNode - Enterprise-Grade IoT Smart Home Automation

![CypherNode Banner](https://img.shields.io/badge/CypherNode-v3.0.1-blue?style=for-the-badge&logo=espressif)
![License](https://img.shields.io/badge/License-MIT-green?style=for-the-badge)
![Platform](https://img.shields.io/badge/Platform-ESP32-red?style=for-the-badge&logo=espressif)

CypherNode is an advanced, highly secure, and highly optimized Smart Home Automation system developed by **Team Cypher-Z**. It moves away from traditional direct-state-mutation IoT models in favor of an **Enterprise-Level Command-Queue Architecture**, ensuring zero data conflicts, strict hardware acknowledgments, and absolute system reliability even on unstable networks.

---

## 🚀 Core Philosophy & Key Features

- **Cloud Command-Queue Architecture:** For cloud interactions, the mobile app pushes commands to a Firebase queue instead of directly altering states. The ESP32 node processes these commands atomically, applies changes, reports the true hardware state back, and then clears the queue (ACK). This guarantees data consistency and eliminates race conditions on unreliable networks.

- **Distributed Local Network:** Nodes form a local mesh network. The embedded web portal can link to other CypherNodes on the LAN, allowing for a unified dashboard and cross-node control without relying on the internet.

- **Intelligent On-Device Configuration:** The embedded web portal provides a rich UI for adding and managing loads. It includes client-side JavaScript validation to prevent GPIO pin collisions (e.g., using the same pin for a relay and a switch) before the configuration is ever sent to the ESP32, ensuring hardware safety.

- **Ghost-State Prevention:** A sophisticated non-blocking debounce algorithm for physical switches ensures instant state synchronization with the cloud. Cloud commands are cross-verified with the hardware's physical state to prevent "ghost overrides" where the app and the device are out of sync.

- **Failsafe Persistence:** Leverages the `LittleFS` file system on the ESP32's flash memory to store all device configurations and the last known states. The system automatically restores its full operational state after a power failure or reboot, even if there is no internet connection.

- **Hyper-Optimized Embedded Web Portal:** A full-featured Single-Page Application (SPA) is served directly from the ESP32's flash memory.
  - **Build Pipeline:** A custom Python script (`html_to_header.py`) automatically minifies the HTML, CSS, and JavaScript, performs Gzip compression, and converts the entire web app into a C++ header file (`WebPage.h`).
  - **Performance:** This process results in a tiny memory footprint and lightning-fast page loads, serving a modern, responsive UI without external dependencies.
  - **Functionality:** The portal allows for direct device control, local load configuration, and linking/management of other nodes on the network.

---

## 📂 Project Structure

```text
CypherNode/
├── CypherNode.ino         # Main Arduino/ESP32 application entry point
├── html_to_header.py      # Python utility to minify & Gzip HTML into a C++ header
├── index.html             # Source code for the Embedded Web Portal
├── WebPage.h              # Auto-generated PROGMEM header containing the Web UI
├── secrets.example.h      # Template for WiFi/Firebase credentials
└── README.md              # Project documentation
```

---

## 🏗️ Technical Architecture

### 1. Hardware Layer (ESP32)

- **Core Logic:** Written in C++ on the Arduino framework for performance and low-level hardware control.
- **Web Server:** Utilizes `ESPAsyncWebServer` to serve the gzipped web portal and handle asynchronous REST API requests with high efficiency.
- **Local REST API:** Exposes a set of endpoints (e.g., `/state`, `/update`, `/get-config`, `/save-config`) for the web portal to interact with the device's configuration and state in real-time.
- **Networking:** Implements `WiFiManager` for dynamic WiFi provisioning via a captive portal and `ESPmDNS` for easy local discovery (e.g., `http://cyphernode.local`).
- **Cloud Communication:** The `Firebase-Client` library manages asynchronous connections, data streams, and atomic updates to the Firebase Realtime Database.
- **Data Handling:** `ArduinoJson` is used for efficient serialization and deserialization of complex JSON objects for both cloud and local API communication.

### 2. Embedded Web Portal Layer

- **Technology Stack:** A lightweight, dependency-free Single-Page Application (SPA) built with vanilla HTML5, CSS3, and JavaScript.
- **UI/UX:** A modern, responsive interface designed for both desktop and mobile browsers, providing a seamless user experience for configuration and control.
- **State Management:**
  - **Runtime State:** JavaScript variables manage the current state of all local and remote devices for the dashboard.
  - **Persistent State:** The browser's `localStorage` is used to remember the connection details (URL, API Key) for linked remote nodes, so they don't need to be re-added on each visit.
- **API Communication:** Uses the standard `fetch` API to make asynchronous GET and POST requests to the ESP32's local REST endpoints.

### 2. Cloud Layer (Firebase)

- **Real-time Synchronization:** Latency-optimized streams for state and command monitoring.
- **Security:** JSON Web Token (JWT) based authentication for the node.
- **Persistence:** Archival of system logs and health heartbeats.

---

## 🗄️ Database Schema (RTDB)

```json
{
  "CypherNode_ID": {
    "config": {
      "load_1": {
        "loadName": "Main Light",
        "loadGPIO": 2,
        "hasPhysicalSwitch": true,
        "switchGPIO": 15,
        "activeHigh": true
      }
    },
    "states": { "load_1": 1 },
    "commands": {
      "toggle": { "load_1": 1 },
      "add": { "load_2": { "loadName": "Fan", "loadGPIO": 4 ... } },
      "system": { "reboot": false }
    },
    "health": {
      "lastPulse": 1710200000,
      "ip_address": "192.168.1.15"
    },
    "sensors": {
      "dht": { "temp": 24.5, "humidity": 60 },
      "vac": { "voltage": 220.5, "current": 1.2 }
    },
    "logs": {
      "unique_id": {
        "message": "[MAC: AA:BB:CC] System Booted",
        "timestamp": 1710200000
      }
    }
  }
}
```

---

## 🛠️ Installation & Setup

1.  **Clone the Repository:**
    ```bash
    git clone https://github.com/nazmuzChakib/CypherNode_PRO.git
    ```
2.  **Configuration:**
    - Rename `secrets.example.h` to `secrets.h`.
    - Fill in your `WIFI_SSID`, `WIFI_PASSWORD`, `FIREBASE_HOST`, and `FIREBASE_AUTH`.
3.  **Web Portal Generation (Optional but Recommended):**
    - If you modify `index.html`, run the Python script to regenerate the header file:
      - Ensure you have Python 3 installed.
      ```bash
      python html_to_header.py
      ```
4.  **Compile & Upload:**
    - Open `CypherNode.ino` in Arduino IDE or VS Code (PlatformIO).
    - Select **ESP32 Dev Module**.
    - Partition Scheme: **Default 4MB with SPIFFS/LittleFS**. A scheme that provides at least 1.5MB for the application and 1.5MB for the filesystem is recommended.
    - Ensure the following libraries are installed: `ESPAsyncWebServer`, `AsyncTCP`, `ArduinoJson`, `Firebase ESP32 Client`, and `LittleFS`.
5.  **First Boot:**
    - If no WiFi is found, the node enters **AP Mode** (SSID: `CypherNode`).
    - Connect and visit `http://192.168.4.1` to configure.

---

## 🤝 Contribution

Developed with ❤️ by **Team Cypher-Z**. Contributions are welcome! Please open an issue or submit a pull request for any optimizations or new sensor drivers.

---

## 📄 License

This project is licensed under the MIT License - see the LICENSE file for details.
