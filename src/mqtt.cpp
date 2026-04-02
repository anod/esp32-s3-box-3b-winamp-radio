// ============================================================
// MQTT Home Assistant integration — implementation
// ============================================================

#include "mqtt.h"
#include <WiFi.h>
#include <PubSubClient.h>

// ─── Command channel (written by MQTT callback, read by main loop) ───

volatile bool mqttCmdPending = false;
volatile MqttCmd mqttCmd = {MQTT_CMD_NONE, 0, false};

// ─── MQTT dirty flag (set from any core, consumed by mqttLoop on Core 1) ───

static volatile bool mqttDirty = false;

// ─── MQTT client ──────────────────────────────────────────

static WiFiClient   mqttWifi;
static PubSubClient mqtt(mqttWifi);

static unsigned long lastReconnectMs = 0;
static const unsigned long RECONNECT_INTERVAL_MS = 5000;

// ─── Topic strings ────────────────────────────────────────

static const char* T_STATE       = MQTT_TOPIC_PREFIX "/state";
static const char* T_AVAIL       = MQTT_TOPIC_PREFIX "/availability";
static const char* T_CMD_VOLUME  = MQTT_TOPIC_PREFIX "/cmd/volume";
static const char* T_CMD_MUTE    = MQTT_TOPIC_PREFIX "/cmd/mute";
static const char* T_CMD_SOURCE  = MQTT_TOPIC_PREFIX "/cmd/source";
static const char* T_CMD_PLAY    = MQTT_TOPIC_PREFIX "/cmd/play";
static const char* T_CMD_PAUSE   = MQTT_TOPIC_PREFIX "/cmd/pause";
static const char* T_CMD_STOP    = MQTT_TOPIC_PREFIX "/cmd/stop";
static const char* T_CMD_NEXT    = MQTT_TOPIC_PREFIX "/cmd/next";
static const char* T_CMD_PREV    = MQTT_TOPIC_PREFIX "/cmd/prev";
static const char* T_DISCOVERY   = "homeassistant/device/" MQTT_CLIENT_ID "/config";

// ─── Testable helpers ─────────────────────────────────────

int mqttVolumeFloatToInt(float vol01) {
    if (vol01 <= 0.0f) return 0;
    if (vol01 >= 1.0f) return MAX_VOLUME;
    return (int)(vol01 * MAX_VOLUME + 0.5f);
}

float mqttVolumeIntToFloat(int volInt) {
    if (volInt <= 0) return 0.0f;
    if (volInt >= MAX_VOLUME) return 1.0f;
    return (float)volInt / (float)MAX_VOLUME;
}

int mqttFindStationByName(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < NUM_STATIONS; i++) {
        if (strcmp(stationNames[i], name) == 0) return i;
    }
    return -1;
}

static const char* playStateStr(int ps) {
    switch (ps) {
        case 1:  return "playing";   // PS_PLAYING
        case 2:  return "paused";    // PS_PAUSED
        default: return "idle";      // PS_STOPPED
    }
}

int mqttBuildStateJson(char *buf, int bufSize) {
    float volF = mqttVolumeIntToFloat(vol);
    bool muted = (vol == 0);
    return snprintf(buf, bufSize,
        "{\"state\":\"%s\","
        "\"volume\":%.2f,"
        "\"is_volume_muted\":%s,"
        "\"media_title\":\"%.*s\","
        "\"source\":\"%s\","
        "\"bitrate\":%ld,"
        "\"output\":\"%s\","
        "\"rssi\":%d,"
        "\"ip\":\"%s\"}",
        playStateStr(playState),
        volF,
        muted ? "true" : "false",
        (int)(sizeof(songTitle) - 1), songTitle,
        (currentStation >= 0 && currentStation < NUM_STATIONS) ? stationNames[currentStation] : "",
        (long)bitrate,
        btMode ? "bt" : "local",
        (int)wifiRssi,
        ipAddress);
}

int mqttBuildDiscoveryJson(char *buf, int bufSize) {
    // Build source options JSON array
    char sources[512];
    int spos = 0;
    spos += snprintf(sources + spos, sizeof(sources) - spos, "[");
    for (int i = 0; i < NUM_STATIONS; i++) {
        int remain = (int)sizeof(sources) - spos;
        if (remain <= 2) break;  // no room for more entries
        if (i > 0) spos += snprintf(sources + spos, remain, ",");
        remain = (int)sizeof(sources) - spos;
        if (remain <= 2) break;
        spos += snprintf(sources + spos, remain, "\"%s\"", stationNames[i]);
    }
    snprintf(sources + spos, sizeof(sources) - spos, "]");

    // HA device discovery: one device, multiple entity components
    return snprintf(buf, bufSize,
        "{"
        "\"dev\":{"
          "\"ids\":[\"%s\"],"
          "\"name\":\"%s\","
          "\"mdl\":\"ESP32-S3-BOX-3\","
          "\"mf\":\"Espressif\""
        "},"
        "\"o\":{\"name\":\"esp32radio\",\"sw\":\"1.0\"},"
        "\"avty_t\":\"%s\","
        "\"stat_t\":\"%s\","

        "\"cmps\":{"

          "\"station\":{"
            "\"p\":\"select\","
            "\"name\":\"Station\","
            "\"uniq_id\":\"%s_station\","
            "\"cmd_t\":\"%s\","
            "\"val_tpl\":\"{{ value_json.source }}\","
            "\"ops\":%s,"
            "\"ic\":\"mdi:radio\""
          "},"

          "\"volume\":{"
            "\"p\":\"number\","
            "\"name\":\"Volume\","
            "\"uniq_id\":\"%s_volume\","
            "\"cmd_t\":\"%s\","
            "\"val_tpl\":\"{{ value_json.volume }}\","
            "\"min\":0,\"max\":1,\"step\":0.05,"
            "\"ic\":\"mdi:volume-high\""
          "},"

          "\"mute\":{"
            "\"p\":\"switch\","
            "\"name\":\"Mute\","
            "\"uniq_id\":\"%s_mute\","
            "\"cmd_t\":\"%s\","
            "\"val_tpl\":\"{{ 'ON' if value_json.is_volume_muted else 'OFF' }}\","
            "\"ic\":\"mdi:volume-off\""
          "},"

          "\"now_playing\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Now Playing\","
            "\"uniq_id\":\"%s_title\","
            "\"val_tpl\":\"{{ value_json.media_title }}\","
            "\"ic\":\"mdi:music-note\""
          "},"

          "\"state\":{"
            "\"p\":\"sensor\","
            "\"name\":\"State\","
            "\"uniq_id\":\"%s_state\","
            "\"val_tpl\":\"{{ value_json.state }}\","
            "\"ic\":\"mdi:play-circle\""
          "},"

          "\"bitrate\":{"
            "\"p\":\"sensor\","
            "\"name\":\"Bitrate\","
            "\"uniq_id\":\"%s_bitrate\","
            "\"val_tpl\":\"{{ value_json.bitrate }}\","
            "\"unit_of_meas\":\"kbps\","
            "\"ic\":\"mdi:speedometer\""
          "},"

          "\"rssi\":{"
            "\"p\":\"sensor\","
            "\"name\":\"WiFi Signal\","
            "\"uniq_id\":\"%s_rssi\","
            "\"val_tpl\":\"{{ value_json.rssi }}\","
            "\"unit_of_meas\":\"dBm\","
            "\"dev_cla\":\"signal_strength\","
            "\"ent_cat\":\"diagnostic\""
          "},"

          "\"play\":{"
            "\"p\":\"button\","
            "\"name\":\"Play\","
            "\"uniq_id\":\"%s_play\","
            "\"cmd_t\":\"%s\","
            "\"ic\":\"mdi:play\""
          "},"

          "\"pause\":{"
            "\"p\":\"button\","
            "\"name\":\"Pause\","
            "\"uniq_id\":\"%s_pause\","
            "\"cmd_t\":\"%s\","
            "\"ic\":\"mdi:pause\""
          "},"

          "\"stop\":{"
            "\"p\":\"button\","
            "\"name\":\"Stop\","
            "\"uniq_id\":\"%s_stop\","
            "\"cmd_t\":\"%s\","
            "\"ic\":\"mdi:stop\""
          "},"

          "\"next\":{"
            "\"p\":\"button\","
            "\"name\":\"Next\","
            "\"uniq_id\":\"%s_next\","
            "\"cmd_t\":\"%s\","
            "\"ic\":\"mdi:skip-next\""
          "},"

          "\"prev\":{"
            "\"p\":\"button\","
            "\"name\":\"Previous\","
            "\"uniq_id\":\"%s_prev\","
            "\"cmd_t\":\"%s\","
            "\"ic\":\"mdi:skip-previous\""
          "}"

        "}"
        "}",
        MQTT_CLIENT_ID, MQTT_DEVICE_NAME,
        T_AVAIL, T_STATE,
        // station select
        MQTT_CLIENT_ID, T_CMD_SOURCE, sources,
        // volume number
        MQTT_CLIENT_ID, T_CMD_VOLUME,
        // mute switch
        MQTT_CLIENT_ID, T_CMD_MUTE,
        // now playing sensor
        MQTT_CLIENT_ID,
        // state sensor
        MQTT_CLIENT_ID,
        // bitrate sensor
        MQTT_CLIENT_ID,
        // rssi sensor
        MQTT_CLIENT_ID,
        // play button
        MQTT_CLIENT_ID, T_CMD_PLAY,
        // pause button
        MQTT_CLIENT_ID, T_CMD_PAUSE,
        // stop button
        MQTT_CLIENT_ID, T_CMD_STOP,
        // next button
        MQTT_CLIENT_ID, T_CMD_NEXT,
        // prev button
        MQTT_CLIENT_ID, T_CMD_PREV);
}

// ─── MQTT callback (runs on Core 1 in PubSubClient.loop()) ──

static void mqttCallback(char *topic, byte *payload, unsigned int length) {
    // Null-terminate payload
    char msg[128];
    int len = (length < sizeof(msg) - 1) ? length : sizeof(msg) - 1;
    memcpy(msg, payload, len);
    msg[len] = '\0';

    Serial.printf("MQTT cmd: %s = %s\n", topic, msg);

    MqttCmd cmd = {MQTT_CMD_NONE, 0, false};

    if (strcmp(topic, T_CMD_VOLUME) == 0) {
        float fv = atof(msg);
        cmd.type = MQTT_CMD_VOLUME;
        cmd.intVal = mqttVolumeFloatToInt(fv);
    } else if (strcmp(topic, T_CMD_MUTE) == 0) {
        cmd.type = MQTT_CMD_MUTE;
        cmd.boolVal = (strcmp(msg, "ON") == 0 || strcmp(msg, "true") == 0);
    } else if (strcmp(topic, T_CMD_SOURCE) == 0) {
        int idx = mqttFindStationByName(msg);
        if (idx >= 0) {
            cmd.type = MQTT_CMD_SOURCE;
            cmd.intVal = idx;
        }
    } else if (strcmp(topic, T_CMD_NEXT) == 0) {
        cmd.type = MQTT_CMD_NEXT;
    } else if (strcmp(topic, T_CMD_PREV) == 0) {
        cmd.type = MQTT_CMD_PREV;
    } else if (strcmp(topic, T_CMD_PLAY) == 0) {
        cmd.type = MQTT_CMD_PLAY;
    } else if (strcmp(topic, T_CMD_PAUSE) == 0) {
        cmd.type = MQTT_CMD_PAUSE;
    } else if (strcmp(topic, T_CMD_STOP) == 0) {
        cmd.type = MQTT_CMD_STOP;
    } else if (strcmp(topic, MQTT_TOPIC_PREFIX "/cmd/playback") == 0) {
        // Unified playback command from HA discovery command_topic
        if (strcmp(msg, "play") == 0) cmd.type = MQTT_CMD_PLAY;
        else if (strcmp(msg, "pause") == 0) cmd.type = MQTT_CMD_PAUSE;
        else if (strcmp(msg, "stop") == 0) cmd.type = MQTT_CMD_STOP;
    }

    if (cmd.type != MQTT_CMD_NONE) {
        mqttCmd.type    = cmd.type;
        mqttCmd.intVal  = cmd.intVal;
        mqttCmd.boolVal = cmd.boolVal;
        mqttCmdPending  = true;
    }
}

// ─── Subscribe to command topics ──────────────────────────

static void mqttSubscribe() {
    mqtt.subscribe(T_CMD_VOLUME);
    mqtt.subscribe(T_CMD_MUTE);
    mqtt.subscribe(T_CMD_SOURCE);
    mqtt.subscribe(T_CMD_PLAY);
    mqtt.subscribe(T_CMD_PAUSE);
    mqtt.subscribe(T_CMD_STOP);
    mqtt.subscribe(T_CMD_NEXT);
    mqtt.subscribe(T_CMD_PREV);
}

// ─── Publish HA discovery + availability ──────────────────

static void mqttPublishDiscovery() {
    char buf[2500];
    int len = mqttBuildDiscoveryJson(buf, sizeof(buf));
    if (len > 0 && len < (int)sizeof(buf)) {
        mqtt.publish(T_DISCOVERY, buf, true);
        Serial.printf("MQTT: discovery published (%d bytes)\n", len);
    } else {
        Serial.printf("MQTT: discovery too large (%d bytes)\n", len);
    }
    mqtt.publish(T_AVAIL, "online", true);
}

// ─── Non-blocking connect ─────────────────────────────────

static bool mqttTryConnect() {
    const char *user = MQTT_USER;
    const char *pass = MQTT_PASSWORD;
    bool hasAuth = (user[0] != '\0');

    Serial.printf("MQTT: connecting to %s:%s...\n", MQTT_BROKER, MQTT_PORT);

    bool ok;
    if (hasAuth) {
        ok = mqtt.connect(MQTT_CLIENT_ID, user, pass, T_AVAIL, 1, true, "offline");
    } else {
        ok = mqtt.connect(MQTT_CLIENT_ID, T_AVAIL, 1, true, "offline");
    }

    if (ok) {
        Serial.println("MQTT: connected");
        mqttSubscribe();
        mqttPublishDiscovery();
        mqttDirty = true;  // publish current state
    } else {
        Serial.printf("MQTT: connect failed (rc=%d)\n", mqtt.state());
    }
    return ok;
}

// ─── Public API ───────────────────────────────────────────

void mqttInit() {
    mqtt.setServer(MQTT_BROKER, atoi(MQTT_PORT));
    mqtt.setBufferSize(2560);
    mqtt.setCallback(mqttCallback);
    mqtt.setKeepAlive(30);

    if (wifiConnected) {
        mqttTryConnect();
        lastReconnectMs = millis();
    }
}

void mqttLoop() {
    if (!wifiConnected) return;

    if (!mqtt.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectMs >= RECONNECT_INTERVAL_MS) {
            lastReconnectMs = now;
            mqttTryConnect();
        }
        return;
    }

    mqtt.loop();

    // Publish state if dirty
    if (mqttDirty) {
        mqttDirty = false;
        char buf[512];
        int len = mqttBuildStateJson(buf, sizeof(buf));
        if (len > 0 && len < (int)sizeof(buf)) {
            mqtt.publish(T_STATE, buf, true);
        }
    }
}

void mqttNotifyStateChange() {
    mqttDirty = true;
}
