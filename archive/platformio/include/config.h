// ============================================================
// Configuration — override via .env file (see README)
// ============================================================

#pragma once

// Wi-Fi credentials
#ifndef WIFI_SSID
#define WIFI_SSID     "YOUR_WIFI_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

// Audio
#define DEFAULT_VOLUME  15   // 0–21
#define MAX_VOLUME      21

// MQTT (Home Assistant integration)
#ifndef MQTT_BROKER
#define MQTT_BROKER   "homeassistant.local"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT     "1883"
#endif
#ifndef MQTT_USER
#define MQTT_USER     ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif

#define MQTT_TOPIC_PREFIX   "esp32radio"
#define MQTT_DEVICE_NAME    "ESP32 Radio"
#define MQTT_CLIENT_ID      "esp32radio"
