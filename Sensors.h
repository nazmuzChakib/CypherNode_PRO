#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>

// ========================================
// DHT11 Sensor setup
// ========================================
#ifdef ENABLE_DHT
    #include <DHT.h>
    #define DHTPIN 4
    #define DHTTYPE DHT11
    DHT dht(DHTPIN, DHTTYPE);
#endif

// ========================================
// INA219 Voltage & Current Sensor setup
// ========================================
#ifdef ENABLE_VAC
    #include <Wire.h>
    #include <Adafruit_INA219.h>
    Adafruit_INA219 ina219;
#endif

// ========================================
// Sensor Initialization
// ========================================
void initSensors() {
    #ifdef ENABLE_DHT
        dht.begin();
        Serial.println("System: DHT11 Sensor Initialized.");
    #endif

    #ifdef ENABLE_VAC
        if (!ina219.begin()) {
            Serial.println("System: INA219 Sensor Init Failed!");
        } else {
            Serial.println("System: INA219 Sensor Initialized.");
        }
    #endif
}

// =========================================
// Read Sensor Data
// =========================================
void readSensorData(float &temp, float &hum, float &vol, float &cur) {
  // default value
  temp = 0.0; 
  hum = 0.0; 
  vol = 0.0; 
  cur = 0.0;

#ifdef ENABLE_DHT
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temp = t;
  if (!isnan(h)) hum = h;
#endif

#ifdef ENABLE_VAC
  vol = ina219.getBusVoltage_V();
  cur = ina219.getCurrent_mA() / 1000.0; 
#endif
}

#endif