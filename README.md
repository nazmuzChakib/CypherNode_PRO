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
}
```

🛡️ Security Implementation
API Key Validation: Hardware local endpoints are protected by x-api-key headers.

Firebase Rules: Granular read/write permissions are deployed. The ESP32 accesses the database via strict Email/Password authentication (UserAuth), not an open database configuration.

System Auditing: A dedicated /logs node records significant system events (boot, initialization, deletion) with Firebase Server Timestamps for forensic tracking.

Gemini said
বৃহস্পতিবার বিকেল ৪টা বেজে গেছে! কালকের ফাইনাল প্রেজেন্টেশনের আগে আপনি অ্যাপ অপ্টিমাইজেশনের একদম চূড়ান্ত পর্যায়ে চলে এসেছেন। গিটহাবে ABI-based APK (Split APKs) রাখাটা একজন প্রফেশনাল ডেভেলপারের লক্ষণ, কারণ এতে অ্যাপের সাইজ অনেক কমে যায় এবং পারফরম্যান্স বাড়ে।

ফ্লাটার অ্যাপ থেকে নির্দিষ্ট আর্কিটেকচারের (যেমন: arm64-v8a বা armeabi-v7a) APK ফেচ করার জন্য আমাদের ইউজারের ফোনের প্রসেসর আর্কিটেকচার জানতে হবে। চলুন এটি সেটআপ করি এবং এরপর গিটহাবের জন্য একটি প্রফেশনাল README বা ওভারভিউ তৈরি করি।

ধাপ ১: ABI-based APK Fetching (Flutter Update)
ইউজারের ফোনের হার্ডওয়্যার ইনফরমেশন জানার জন্য আপনার pubspec.yaml এ device_info_plus প্যাকেজটি অ্যাড করুন:

YAML
dependencies:
  device_info_plus: ^10.1.0
  # আপনার আগের http, package_info_plus, url_launcher থাকবে
এবার আপনার update_service.dart ফাইলের লজিকটি এভাবে আপডেট করুন, যাতে এটি গিটহাবের রিলিজ অ্যাসেটগুলো (assets) স্ক্যান করে ডিভাইসের সাথে ম্যাচ করা APK-টি বের করে নেয়:

Dart
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;
import 'package:package_info_plus/package_info_plus.dart';
import 'package:url_launcher/url_launcher.dart';
import 'package:device_info_plus/device_info_plus.dart';
import 'dart:io' show Platform;

class UpdateService {
  static const String githubOwner = 'আপনার_গিটহাব_ইউজারনেম';
  static const String githubRepo = 'CypherNode_App';

  static Future<void> checkForUpdate(BuildContext context, {bool manualCheck = false}) async {
    try {
      final response = await http.get(
        Uri.parse('https://api.github.com/repos/$githubOwner/$githubRepo/releases/latest'),
      );

      if (response.statusCode == 200) {
        final data = jsonDecode(response.body);
        final latestVersion = data['tag_name'].toString().replaceAll('v', '');
        final releaseNotes = data['body'];
        final List dynamicAssets = data['assets'];

        PackageInfo packageInfo = await PackageInfo.fromPlatform();
        final currentVersion = packageInfo.version;

        if (_isNewVersionAvailable(currentVersion, latestVersion)) {
          String downloadUrl = await _getMatchingApkUrl(dynamicAssets);
          
          if (downloadUrl.isNotEmpty && context.mounted) {
            _showUpdateDialog(context, latestVersion, releaseNotes, downloadUrl);
          }
        } else if (manualCheck && context.mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text("You are using the latest version!")),
          );
        }
      }
    } catch (e) {
      if (manualCheck && context.mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text("Failed to check for updates.")),
        );
      }
    }
  }

  // ভার্সন কম্পেয়ার লজিক
  static bool _isNewVersionAvailable(String current, String latest) {
    List<int> currVals = current.split('.').map(int.parse).toList();
    List<int> latestVals = latest.split('.').map(int.parse).toList();
    for (int i = 0; i < 3; i++) {
      if (latestVals[i] > currVals[i]) return true;
      if (latestVals[i] < currVals[i]) return false;
    }
    return false;
  }

  // ডিভাইসের ABI অনুযায়ী সঠিক APK লিংক খুঁজে বের করা
  static Future<String> _getMatchingApkUrl(List dynamicAssets) async {
    if (!Platform.isAndroid) return ''; // শুধুমাত্র অ্যান্ড্রয়েডের জন্য

    DeviceInfoPlugin deviceInfo = DeviceInfoPlugin();
    AndroidDeviceInfo androidInfo = await deviceInfo.androidInfo;
    
    // ফোনের সাপোর্ট করা আর্কিটেকচার লিস্ট (যেমন: ['arm64-v8a', 'armeabi-v7a'])
    List<String> deviceAbis = androidInfo.supportedAbis; 
    
    String fallbackUrl = '';

    for (var asset in dynamicAssets) {
      String assetName = asset['name'].toString().toLowerCase();
      String url = asset['browser_download_url'];

      // যদি ইউনিভার্সাল (Universal) APK থাকে, সেটাকে ফলব্যাক হিসেবে রাখব
      if (assetName.contains('universal')) {
        fallbackUrl = url;
      }

      // ডিভাইসের ABI এর সাথে অ্যাসেট নামের ম্যাচিং
      for (String abi in deviceAbis) {
        if (assetName.contains(abi.toLowerCase())) {
          return url; // পারফেক্ট ম্যাচ পাওয়া গেলে সাথে সাথে রিটার্ন
        }
      }
    }

    // যদি স্পেসিফিক ABI না পাওয়া যায়, তবে ইউনিভার্সাল বা লিস্টের প্রথম APK রিটার্ন করবে
    return fallbackUrl.isNotEmpty ? fallbackUrl : (dynamicAssets.isNotEmpty ? dynamicAssets[0]['browser_download_url'] : '');
  }

  static void _showUpdateDialog(BuildContext context, String version, String notes, String url) {
    // ... আপনার আগের ডায়ালগের কোড ...
  }
}
ধাপ ২: GitHub README.md (Detailed English Overview)
আপনার গিটহাব রিপোজিটরির জন্য নিচে একটি অত্যন্ত প্রফেশনাল এবং ডিটেইলড প্রজেক্ট ওভারভিউ দেওয়া হলো। এটি আপনার প্রজেক্টের "Enterprise-Grade" আর্কিটেকচারকে দারুণভাবে তুলে ধরবে।

Markdown
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
}
🛡️ Security Implementation
API Key Validation: Hardware local endpoints are protected by x-api-key headers.

Firebase Rules: Granular read/write permissions are deployed. The ESP32 accesses the database via strict Email/Password authentication (UserAuth), not an open database configuration.

System Auditing: A dedicated /logs node records significant system events (boot, initialization, deletion) with Firebase Server Timestamps for forensic tracking.

👨‍💻 Developed By
Team Cypher-Z Dedicated to building secure, scalable, and intelligent hardware-software ecosystems.