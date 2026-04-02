// ============================================================
// Native unit tests for MQTT helpers
// Runs on host via: pio test -e native
// ============================================================

#include <unity.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

// ─── Stubs for globals normally defined in main.cpp ───────

#define DEFAULT_VOLUME  15
#define MAX_VOLUME      21

#define MQTT_TOPIC_PREFIX  "esp32radio"
#define MQTT_DEVICE_NAME   "ESP32 Radio"
#define MQTT_CLIENT_ID     "esp32radio"

#define NUM_STATIONS 10
const char* stationUrls[NUM_STATIONS] = {
    "http://a", "http://b", "http://c", "http://d", "http://e",
    "http://f", "http://g", "http://h", "http://i", "http://j"
};
const char* stationNames[NUM_STATIONS] = {
    "Groove Salad", "The Trip", "Psytrance", "Rock Antenne", "181 Hard Rock",
    "Heavy Metal", "181 Power Hits", "Radio Paradise", "BBC World News", "NPR News"
};

volatile int  currentStation = 0;
volatile int  vol            = 15;
char          songTitle[128] = "Test Song - Test Artist";
volatile long bitrate        = 128;
volatile bool wifiConnected  = true;
volatile int  wifiRssi       = -52;
volatile bool btMode         = false;
char          id3Artist[64]  = "Test Artist";
char          id3Title[64]   = "Test Song";
char          ipAddress[20]  = "192.168.1.42";
volatile int  playState      = 1;  // PS_PLAYING

// ─── Include the functions under test ─────────────────────
// We include the testable helpers directly; they depend only
// on the globals above and <cstdio>/<cstring>.

// Paste the helper functions from mqtt.cpp to avoid pulling
// in PubSubClient/WiFi deps in native builds.

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
        case 1:  return "playing";
        case 2:  return "paused";
        default: return "idle";
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
    char sources[512];
    int spos = 0;
    spos += snprintf(sources + spos, sizeof(sources) - spos, "[");
    for (int i = 0; i < NUM_STATIONS; i++) {
        if (i > 0) spos += snprintf(sources + spos, sizeof(sources) - spos, ",");
        spos += snprintf(sources + spos, sizeof(sources) - spos, "\"%s\"", stationNames[i]);
    }
    snprintf(sources + spos, sizeof(sources) - spos, "]");

    return snprintf(buf, bufSize,
        "{"
        "\"dev\":{"
          "\"ids\":[\"%s\"],"
          "\"name\":\"%s\","
          "\"mdl\":\"ESP32-S3-BOX-3\","
          "\"mf\":\"Espressif\""
        "},"
        "\"o\":{\"name\":\"esp32radio\",\"sw\":\"1.0\"},"
        "\"avty_t\":\"%s/availability\","
        "\"stat_t\":\"%s/state\","
        "\"cmps\":{"
          "\"station\":{\"p\":\"select\",\"name\":\"Station\",\"uniq_id\":\"%s_station\",\"cmd_t\":\"%s/cmd/source\",\"val_tpl\":\"{{ value_json.source }}\",\"ops\":%s,\"ic\":\"mdi:radio\"},"
          "\"volume\":{\"p\":\"number\",\"name\":\"Volume\",\"uniq_id\":\"%s_volume\",\"cmd_t\":\"%s/cmd/volume\",\"val_tpl\":\"{{ value_json.volume }}\",\"min\":0,\"max\":1,\"step\":0.05,\"ic\":\"mdi:volume-high\"},"
          "\"mute\":{\"p\":\"switch\",\"name\":\"Mute\",\"uniq_id\":\"%s_mute\",\"cmd_t\":\"%s/cmd/mute\",\"val_tpl\":\"{{ 'ON' if value_json.is_volume_muted else 'OFF' }}\",\"ic\":\"mdi:volume-off\"},"
          "\"now_playing\":{\"p\":\"sensor\",\"name\":\"Now Playing\",\"uniq_id\":\"%s_title\",\"val_tpl\":\"{{ value_json.media_title }}\",\"ic\":\"mdi:music-note\"},"
          "\"state\":{\"p\":\"sensor\",\"name\":\"State\",\"uniq_id\":\"%s_state\",\"val_tpl\":\"{{ value_json.state }}\",\"ic\":\"mdi:play-circle\"},"
          "\"bitrate\":{\"p\":\"sensor\",\"name\":\"Bitrate\",\"uniq_id\":\"%s_bitrate\",\"val_tpl\":\"{{ value_json.bitrate }}\",\"unit_of_meas\":\"kbps\",\"ic\":\"mdi:speedometer\"},"
          "\"rssi\":{\"p\":\"sensor\",\"name\":\"WiFi Signal\",\"uniq_id\":\"%s_rssi\",\"val_tpl\":\"{{ value_json.rssi }}\",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"ent_cat\":\"diagnostic\"},"
          "\"play\":{\"p\":\"button\",\"name\":\"Play\",\"uniq_id\":\"%s_play\",\"cmd_t\":\"%s/cmd/play\",\"ic\":\"mdi:play\"},"
          "\"pause\":{\"p\":\"button\",\"name\":\"Pause\",\"uniq_id\":\"%s_pause\",\"cmd_t\":\"%s/cmd/pause\",\"ic\":\"mdi:pause\"},"
          "\"stop\":{\"p\":\"button\",\"name\":\"Stop\",\"uniq_id\":\"%s_stop\",\"cmd_t\":\"%s/cmd/stop\",\"ic\":\"mdi:stop\"},"
          "\"next\":{\"p\":\"button\",\"name\":\"Next\",\"uniq_id\":\"%s_next\",\"cmd_t\":\"%s/cmd/next\",\"ic\":\"mdi:skip-next\"},"
          "\"prev\":{\"p\":\"button\",\"name\":\"Previous\",\"uniq_id\":\"%s_prev\",\"cmd_t\":\"%s/cmd/prev\",\"ic\":\"mdi:skip-previous\"}"
        "}"
        "}",
        MQTT_CLIENT_ID, MQTT_DEVICE_NAME,
        MQTT_TOPIC_PREFIX, MQTT_TOPIC_PREFIX,
        MQTT_CLIENT_ID, MQTT_TOPIC_PREFIX, sources,
        MQTT_CLIENT_ID, MQTT_TOPIC_PREFIX,
        MQTT_CLIENT_ID, MQTT_TOPIC_PREFIX,
        MQTT_CLIENT_ID,
        MQTT_CLIENT_ID,
        MQTT_CLIENT_ID,
        MQTT_CLIENT_ID,
        MQTT_CLIENT_ID, MQTT_TOPIC_PREFIX,
        MQTT_CLIENT_ID, MQTT_TOPIC_PREFIX,
        MQTT_CLIENT_ID, MQTT_TOPIC_PREFIX,
        MQTT_CLIENT_ID, MQTT_TOPIC_PREFIX,
        MQTT_CLIENT_ID, MQTT_TOPIC_PREFIX);
}

// ─── Volume conversion tests ─────────────────────────────

void test_volumeFloatToInt_zero() {
    TEST_ASSERT_EQUAL_INT(0, mqttVolumeFloatToInt(0.0f));
}

void test_volumeFloatToInt_max() {
    TEST_ASSERT_EQUAL_INT(MAX_VOLUME, mqttVolumeFloatToInt(1.0f));
}

void test_volumeFloatToInt_mid() {
    int result = mqttVolumeFloatToInt(0.5f);
    TEST_ASSERT_TRUE(result == 10 || result == 11);
}

void test_volumeFloatToInt_clamp_above() {
    TEST_ASSERT_EQUAL_INT(MAX_VOLUME, mqttVolumeFloatToInt(1.5f));
}

void test_volumeFloatToInt_clamp_below() {
    TEST_ASSERT_EQUAL_INT(0, mqttVolumeFloatToInt(-0.1f));
}

void test_volumeIntToFloat_zero() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, mqttVolumeIntToFloat(0));
}

void test_volumeIntToFloat_max() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, mqttVolumeIntToFloat(MAX_VOLUME));
}

void test_volumeIntToFloat_default() {
    float result = mqttVolumeIntToFloat(15);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.71f, result);
}

// ─── Station lookup tests ─────────────────────────────────

void test_findStation_first() {
    TEST_ASSERT_EQUAL_INT(0, mqttFindStationByName("Groove Salad"));
}

void test_findStation_last() {
    TEST_ASSERT_EQUAL_INT(9, mqttFindStationByName("NPR News"));
}

void test_findStation_unknown() {
    TEST_ASSERT_EQUAL_INT(-1, mqttFindStationByName("Unknown Station"));
}

void test_findStation_empty() {
    TEST_ASSERT_EQUAL_INT(-1, mqttFindStationByName(""));
}

void test_findStation_null() {
    TEST_ASSERT_EQUAL_INT(-1, mqttFindStationByName(NULL));
}

// ─── Play state string tests ──────────────────────────────

void test_playStateStr_playing() {
    TEST_ASSERT_EQUAL_STRING("playing", playStateStr(1));
}

void test_playStateStr_paused() {
    TEST_ASSERT_EQUAL_STRING("paused", playStateStr(2));
}

void test_playStateStr_stopped() {
    TEST_ASSERT_EQUAL_STRING("idle", playStateStr(0));
}

// ─── State JSON tests ─────────────────────────────────────

void test_buildStateJson_basic() {
    char buf[512];
    int len = mqttBuildStateJson(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_LESS_THAN((int)sizeof(buf), len);

    // Verify key fields present
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"state\":\"playing\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"source\":\"Groove Salad\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"bitrate\":128"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"output\":\"local\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ip\":\"192.168.1.42\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"is_volume_muted\":false"));
}

void test_buildStateJson_muted() {
    int origVol = vol;
    vol = 0;
    char buf[512];
    mqttBuildStateJson(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"is_volume_muted\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"volume\":0.00"));
    vol = origVol;
}

void test_buildStateJson_empty_title() {
    char origTitle[128];
    strncpy(origTitle, songTitle, sizeof(origTitle));
    songTitle[0] = '\0';

    char buf[512];
    mqttBuildStateJson(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"media_title\":\"\""));

    strncpy(songTitle, origTitle, sizeof(songTitle));
}

void test_buildStateJson_paused() {
    int origState = playState;
    playState = 2;  // PS_PAUSED
    char buf[512];
    mqttBuildStateJson(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"state\":\"paused\""));
    playState = origState;
}

void test_buildStateJson_bt_output() {
    bool origBt = btMode;
    btMode = true;
    char buf[512];
    mqttBuildStateJson(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"output\":\"bt\""));
    btMode = origBt;
}

// ─── Discovery JSON tests ─────────────────────────────────

void test_buildDiscoveryJson_fitsBuffer() {
    char buf[2500];
    int len = mqttBuildDiscoveryJson(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_LESS_THAN(2500, len);
}

void test_buildDiscoveryJson_requiredFields() {
    char buf[2500];
    mqttBuildDiscoveryJson(buf, sizeof(buf));

    // Device discovery required fields
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"ESP32 Radio\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ids\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dev\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"o\""));  // origin required
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmps\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"avty_t\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_t\""));
    // Component unique_ids
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"uniq_id\":\"esp32radio_station\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"uniq_id\":\"esp32radio_volume\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"uniq_id\":\"esp32radio_mute\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"uniq_id\":\"esp32radio_title\""));
}

void test_buildDiscoveryJson_allStations() {
    char buf[2500];
    mqttBuildDiscoveryJson(buf, sizeof(buf));

    for (int i = 0; i < NUM_STATIONS; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(
            strstr(buf, stationNames[i]),
            stationNames[i]);
    }
}

void test_buildDiscoveryJson_components() {
    char buf[2500];
    mqttBuildDiscoveryJson(buf, sizeof(buf));

    // Verify component platforms
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"p\":\"select\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"p\":\"number\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"p\":\"switch\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"p\":\"sensor\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"p\":\"button\""));
}

// ─── Test runner ──────────────────────────────────────────

int main(int argc, char **argv) {
    UNITY_BEGIN();

    // Volume conversions
    RUN_TEST(test_volumeFloatToInt_zero);
    RUN_TEST(test_volumeFloatToInt_max);
    RUN_TEST(test_volumeFloatToInt_mid);
    RUN_TEST(test_volumeFloatToInt_clamp_above);
    RUN_TEST(test_volumeFloatToInt_clamp_below);
    RUN_TEST(test_volumeIntToFloat_zero);
    RUN_TEST(test_volumeIntToFloat_max);
    RUN_TEST(test_volumeIntToFloat_default);

    // Station lookup
    RUN_TEST(test_findStation_first);
    RUN_TEST(test_findStation_last);
    RUN_TEST(test_findStation_unknown);
    RUN_TEST(test_findStation_empty);
    RUN_TEST(test_findStation_null);

    // Play state
    RUN_TEST(test_playStateStr_playing);
    RUN_TEST(test_playStateStr_paused);
    RUN_TEST(test_playStateStr_stopped);

    // State JSON
    RUN_TEST(test_buildStateJson_basic);
    RUN_TEST(test_buildStateJson_muted);
    RUN_TEST(test_buildStateJson_empty_title);
    RUN_TEST(test_buildStateJson_paused);
    RUN_TEST(test_buildStateJson_bt_output);

    // Discovery JSON
    RUN_TEST(test_buildDiscoveryJson_fitsBuffer);
    RUN_TEST(test_buildDiscoveryJson_requiredFields);
    RUN_TEST(test_buildDiscoveryJson_allStations);
    RUN_TEST(test_buildDiscoveryJson_components);

    return UNITY_END();
}
