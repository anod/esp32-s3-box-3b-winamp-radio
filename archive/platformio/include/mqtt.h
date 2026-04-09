// ============================================================
// MQTT Home Assistant integration
// ============================================================

#pragma once

#include <Arduino.h>
#include "config.h"

// Shared state (defined in main.cpp, read by MQTT module)
#define NUM_STATIONS 10
extern const char*   stationUrls[NUM_STATIONS];
extern const char*   stationNames[NUM_STATIONS];
extern volatile int  currentStation;
extern volatile int  vol;
extern char          songTitle[128];
extern volatile long bitrate;
extern volatile bool wifiConnected;
extern volatile int  wifiRssi;
extern volatile bool    btMode;
extern volatile uint8_t brightness;
extern volatile bool    screenOn;
extern char          id3Artist[64];
extern char          id3Title[64];
extern char          ipAddress[20];

// Playback state enum (defined in main.cpp)
enum PlayState : int;
extern volatile int  playState;

// MQTT command types
enum MqttCmdType {
    MQTT_CMD_NONE = 0,
    MQTT_CMD_VOLUME,
    MQTT_CMD_MUTE,
    MQTT_CMD_SOURCE,
    MQTT_CMD_NEXT,
    MQTT_CMD_PREV,
    MQTT_CMD_PLAY,
    MQTT_CMD_PAUSE,
    MQTT_CMD_STOP,
    MQTT_CMD_BRIGHTNESS,
    MQTT_CMD_SCREEN
};

struct MqttCmd {
    MqttCmdType type;
    int         intVal;
    bool        boolVal;
};

// Command pending flag — set by MQTT callback, consumed by main loop
extern volatile bool mqttCmdPending;
extern volatile MqttCmd mqttCmd;

// Public API
void mqttInit();
void mqttLoop();
void mqttNotifyStateChange();

// Testable helpers (also used by native unit tests)
int  mqttBuildStateJson(char *buf, int bufSize);
int  mqttBuildDiscoveryJson(char *buf, int bufSize);
int  mqttVolumeFloatToInt(float vol01);
float mqttVolumeIntToFloat(int volInt);
int  mqttFindStationByName(const char *name);
