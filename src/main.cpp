// ============================================================
// ESP32-S3-BOX-3 Internet Radio
// Based on VolosR/WaveshareRadioStream, adapted for ESP32-S3-BOX-3
// Display: 320x240 ILI9342C | Audio: ES8311 | Touch: GT911
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <Audio.h>
#include <Preferences.h>
#define LGFX_USE_V1
#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>
#include <Wire.h>
#include "config.h"
#include "pins.h"
#include "es8311.h"

// ─── Globals ──────────────────────────────────────────────

static LGFX       tft;
static LGFX_Sprite canvas(&tft);
static Audio       audio;
static Preferences prefs;

// Station list
#define NUM_STATIONS 10
static const char* stationUrls[NUM_STATIONS] = {
    "http://ice1.somafm.com/groovesalad-128-mp3",            // Electronic / Chill
    "http://ice1.somafm.com/thetrip-128-mp3",                // Trance / Progressive
    "http://hirschmilch.de:7000/psytrance.mp3",              // Psytrance
    "http://stream.rockantenne.de/rockantenne/stream/mp3",   // Rock
    "http://listen.181fm.com/181-hardrock_128k.mp3",         // Hard Rock
    "http://stream.rockantenne.de/heavy-metal/stream/mp3",   // Metal
    "http://listen.181fm.com/181-power_128k.mp3",            // Pop / Top 40
    "http://stream.radioparadise.com/mp3-128",               // Eclectic Mix
    "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service", // News (BBC)
    "http://npr-ice.streamguys1.com/live.mp3"                // News (NPR)
};
static const char* stationNames[NUM_STATIONS] = {
    "Groove Salad",
    "The Trip",
    "Psytrance",
    "Rock Antenne",
    "181 Hard Rock",
    "Heavy Metal",
    "181 Power Hits",
    "Radio Paradise",
    "BBC World News",
    "NPR News"
};

// State (shared between Core 0 audio task and Core 1 render loop)
static volatile int    currentStation  = 0;
static volatile int    vol             = DEFAULT_VOLUME;
static char            songTitle[128]  = "";
static volatile long   bitrate         = 0;
static volatile bool   wifiConnected   = false;
static volatile int    wifiRssi        = 0;
static int             songScrollX     = 320;
static char            id3Artist[64]   = "";
static char            id3Title[64]    = "";
static char            ipAddress[20]   = "";

// Timing
static unsigned long lastFrameMs   = 0;
static unsigned long lastStatusMs  = 0;
static unsigned long lastTouchMs   = 0;
static unsigned long lastVisMs     = 0;

// Visualiser bars
static int graphBars[12] = {0};
static uint16_t barColors[6];  // pre-computed gradient

// UI layout constants
static constexpr int LPANEL_W   = 198;
static constexpr int RPANEL_X   = 202;
static constexpr int RPANEL_W   = 116;
static constexpr int HDR_H      = 16;
static constexpr int STA_Y      = 20;
static constexpr int STA_LINE   = 18;
static constexpr int TICKER_Y   = 220;
static constexpr int TICKER_H   = 18;
static constexpr int TICKER_REGION_Y = 200;   // top of ticker region (includes label)
static constexpr int TICKER_REGION_H = 40;    // full height to bottom of screen

// Theme colours (computed once in setup)
static uint16_t cBg, cPanel, cBorder, cAccent, cDim, cBright;
static uint16_t cStationHl, cStationDim, cBarBg, cBtnCol;

// Retained ES8311 handle (avoid leak)
static es8311_handle_t esHandle = nullptr;

// Forward declarations
void audioCallback(Audio::msg_t msg);
static void audioTask(void *param);

static void saveStation() {
    prefs.begin("radio", false);
    prefs.putInt("station", currentStation);
    prefs.putInt("volume", vol);
    prefs.end();
}

static void loadStation() {
    prefs.begin("radio", true);
    currentStation = prefs.getInt("station", 0);
    vol = prefs.getInt("volume", DEFAULT_VOLUME);
    prefs.end();
    if (currentStation < 0 || currentStation >= NUM_STATIONS) currentStation = 0;
    if (vol < 0 || vol > MAX_VOLUME) vol = DEFAULT_VOLUME;
    Serial.printf("Restored station %d, volume %d\n", currentStation, vol);
}

// ─── ES8311 codec init ───────────────────────────────────

static bool initCodec() {
    esHandle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!esHandle) { Serial.println("ES8311 create failed"); return false; }

    const es8311_clock_config_t clk = {
        .mclk_inverted    = false,
        .sclk_inverted    = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency   = ES8311_MCLK_FREQ_HZ,
        .sample_frequency = ES8311_SAMPLE_RATE
    };

    if (es8311_init(esHandle, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        Serial.println("ES8311 init failed");
        return false;
    }
    es8311_sample_frequency_config(esHandle, ES8311_MCLK_FREQ_HZ, ES8311_SAMPLE_RATE);
    es8311_voice_volume_set(esHandle, ES8311_VOICE_VOLUME, NULL);
    es8311_microphone_config(esHandle, false);
    Serial.println("ES8311 codec OK");
    return true;
}

// ─── Drawing (single canvas, one push per frame) ─────────

static void drawFrame() {
    canvas.fillSprite(cBg);

    // ── Left panel: station list ──
    canvas.fillRect(2, HDR_H + 2, LPANEL_W - 4, NUM_STATIONS * STA_LINE + 8, cPanel);
    canvas.drawRect(2, HDR_H + 2, LPANEL_W - 4, NUM_STATIONS * STA_LINE + 8, cBorder);
    canvas.fillRect(4, HDR_H, LPANEL_W - 8, 2, cAccent);

    canvas.setTextColor(cBright, cBg);
    canvas.drawString("STATIONS", 56, 2, 2);
    canvas.drawString("INTERNET RADIO", RPANEL_X, 2, 2);

    for (int i = 0; i < NUM_STATIONS; i++) {
        int y = STA_Y + 4 + i * STA_LINE;
        if (i == currentStation) {
            canvas.fillRect(4, y - 1, LPANEL_W - 8, STA_LINE - 2, cStationHl);
            canvas.setTextColor(TFT_GREEN, cStationHl);
            canvas.drawString(">", 8, y, 2);
        } else {
            canvas.setTextColor(cStationDim, cPanel);
        }
        canvas.drawString(stationNames[i], 24, y, 2);
    }

    canvas.fillRect(LPANEL_W, HDR_H + 2, 3, NUM_STATIONS * STA_LINE + 8, cBorder);

    // ── Right panel: WiFi ──
    int ry = HDR_H + 2;
    canvas.fillRect(RPANEL_X, ry, RPANEL_W, 56, cPanel);
    canvas.drawRect(RPANEL_X, ry, RPANEL_W, 56, cBorder);
    canvas.fillRect(RPANEL_X, HDR_H, RPANEL_W, 2, cAccent);

    canvas.setTextColor(cDim, cPanel);
    canvas.drawString("WIFI", RPANEL_X + 4, ry + 4, 1);
    canvas.setTextColor(wifiConnected ? TFT_GREEN : TFT_RED, cPanel);
    canvas.drawString(wifiConnected ? "CONNECTED" : "OFFLINE", RPANEL_X + 4, ry + 16, 1);
    canvas.setTextColor(cBright, cPanel);
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "RSSI:%d", wifiRssi);
        canvas.drawString(buf, RPANEL_X + 4, ry + 28, 1);
    }
    if (wifiConnected) {
        canvas.setTextColor(cDim, cPanel);
        canvas.drawString(ipAddress, RPANEL_X + 4, ry + 40, 1);
    }

    // ── Right panel: Visualiser ──
    int gy = ry + 60;
    canvas.fillRect(RPANEL_X, gy, RPANEL_W, 38, cPanel);
    canvas.drawRect(RPANEL_X, gy, RPANEL_W, 38, cBorder);
    for (int i = 0; i < 12; i++) {
        for (int j = 0; j < graphBars[i]; j++) {
            canvas.fillRect(RPANEL_X + 6 + i * 9, gy + 30 - j * 6, 7, 5, barColors[j]);
        }
    }

    // ── Right panel: Volume ──
    int vy = gy + 42;
    canvas.fillRect(RPANEL_X, vy, RPANEL_W, 42, cPanel);
    canvas.drawRect(RPANEL_X, vy, RPANEL_W, 42, cBorder);

    canvas.setTextColor(cDim, cPanel);
    canvas.drawString("VOLUME", RPANEL_X + 32, vy + 2, 1);

    int barW = RPANEL_W - 16;
    int barX = RPANEL_X + 8;
    int barY = vy + 14;
    canvas.fillRoundRect(barX, barY, barW, 4, 2, cBarBg);
    int fillW = (vol * barW) / MAX_VOLUME;
    canvas.fillRoundRect(barX, barY, fillW, 4, 2, TFT_YELLOW);
    int knobX = barX + fillW - 4;
    if (knobX < barX) knobX = barX;
    canvas.fillRoundRect(knobX, barY - 4, 8, 12, 3, cBright);

    uint16_t muteCol = (vol == 0) ? TFT_RED : cBtnCol;
    canvas.fillRoundRect(RPANEL_X + 4,  vy + 24, 33, 15, 3, cBtnCol);
    canvas.fillRoundRect(RPANEL_X + 41, vy + 24, 33, 15, 3, muteCol);
    canvas.fillRoundRect(RPANEL_X + 78, vy + 24, 33, 15, 3, cBtnCol);
    canvas.setTextColor(cBright, cBtnCol);
    canvas.drawString(" - ", RPANEL_X + 10,  vy + 27, 1);
    canvas.setTextColor(cBright, muteCol);
    canvas.drawString("MUTE", RPANEL_X + 44, vy + 27, 1);
    canvas.setTextColor(cBright, cBtnCol);
    canvas.drawString(" + ", RPANEL_X + 87, vy + 27, 1);

    // ── Right panel: Bitrate ──
    int by = vy + 46;
    canvas.fillRect(RPANEL_X, by, RPANEL_W, 16, cPanel);
    canvas.drawRect(RPANEL_X, by, RPANEL_W, 16, cBorder);
    canvas.setTextColor(TFT_GREEN, cPanel);
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "BITRATE %ldk", bitrate);
        canvas.drawString(buf, RPANEL_X + 4, by + 3, 1);
    }

    // ── Song title area ──
    canvas.fillRect(2, TICKER_REGION_Y, 316, 14, cBg);
    canvas.setTextColor(cDim, cBg);
    canvas.drawString("NOW PLAYING", 6, TICKER_REGION_Y + 2, 1);

    canvas.fillRect(2, TICKER_Y - 4, 316, TICKER_H + 6, cPanel);
    canvas.drawRect(2, TICKER_Y - 4, 316, TICKER_H + 6, cBorder);
    canvas.setTextColor(cBright, cPanel);
    canvas.drawString(songTitle, songScrollX + 4, TICKER_Y - 2, 2);

    // Outer frame
    canvas.drawRect(0, 0, 320, 240, cBorder);

    // Single push — no flicker
    canvas.pushSprite(0, 0);
}

// ─── Input handling ───────────────────────────────────────

static int  savedVol = DEFAULT_VOLUME;

static void handleTouch() {
    lgfx::touch_point_t tp;
    if (!tft.getTouch(&tp)) return;

    unsigned long now = millis();
    if (now - lastTouchMs < 300) return;
    lastTouchMs = now;

    int tx = tp.x, ty = tp.y;

    // Station selection — left panel
    if (tx < LPANEL_W && ty > STA_Y && ty < STA_Y + NUM_STATIONS * STA_LINE + 10) {
        int idx = (ty - STA_Y - 4) / STA_LINE;
        if (idx >= 0 && idx < NUM_STATIONS && idx != currentStation) {
            currentStation = idx;
            strlcpy(songTitle, "Connecting...", sizeof(songTitle));
            id3Artist[0] = '\0'; id3Title[0] = '\0'; bitrate = 0;
            songScrollX = 10;
            audio.connecttohost(stationUrls[currentStation]);
            Serial.printf("Station: %s\n", stationNames[currentStation]);
            saveStation();
        }
    }

    // Volume/Mute buttons — right panel (generous hit area for touch offset)
    int volPanelY = HDR_H + 2 + 60 + 42;  // vy = 120
    if (tx > RPANEL_X && ty >= volPanelY + 10 && ty <= volPanelY + 52) {
        if (tx < RPANEL_X + 37 && vol > 0) {
            vol--;
            audio.setVolume(vol);
            saveStation();
        }
        else if (tx >= RPANEL_X + 38 && tx < RPANEL_X + 76) {
            if (vol > 0) { savedVol = vol; vol = 0; }
            else         { vol = savedVol; }
            audio.setVolume(vol);
            saveStation();
        }
        else if (tx >= RPANEL_X + 76 && vol < MAX_VOLUME) {
            vol++;
            audio.setVolume(vol);
            saveStation();
        }
    }
}

static bool prevBoot = false;

static void handleButtons() {
    // Boot button — next station
    bool boot = (digitalRead(BTN_BOOT) == LOW);
    if (boot && !prevBoot) {
        currentStation = (currentStation + 1) % NUM_STATIONS;
        strlcpy(songTitle, "Connecting...", sizeof(songTitle));
        id3Artist[0] = '\0'; id3Title[0] = '\0'; bitrate = 0;
        songScrollX = 10;
        audio.connecttohost(stationUrls[currentStation]);
        saveStation();
    }
    prevBoot = boot;
}

// ─── Setup ────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== ESP32-S3-BOX-3 Internet Radio ===");

    loadStation();

    // I2C for ES8311 codec (port 0, shared with touch)
    Wire.begin(I2C_SDA, I2C_SCL);

    pinMode(BTN_BOOT, INPUT_PULLUP);

    // Keep PA off until audio is ready to avoid noise
    pinMode(PA_PIN, OUTPUT);
    digitalWrite(PA_PIN, LOW);

    initCodec();

    // Display
    tft.init();
    tft.setBrightness(160);
    tft.fillScreen(TFT_BLACK);

    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(320, 240);

    cBg        = canvas.color565(50, 50, 60);
    cPanel     = TFT_BLACK;
    cBorder    = canvas.color565(100, 100, 120);
    cAccent    = TFT_ORANGE;
    cDim       = canvas.color565(120, 120, 120);
    cBright    = canvas.color565(220, 220, 255);
    cStationHl = canvas.color565(0, 40, 0);
    cStationDim= canvas.color565(0, 140, 0);
    cBarBg     = canvas.color565(60, 60, 60);
    cBtnCol    = canvas.color565(80, 80, 100);
    for (int j = 0; j < 6; j++)
        barColors[j] = canvas.color565(0, 100 + j * 30, 50 + j * 20);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(40, 100);
    tft.println("Connecting WiFi...");
    tft.setTextSize(1);
    tft.setCursor(40, 130);
    tft.println(WIFI_SSID);

    // WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
        strlcpy(ipAddress, WiFi.localIP().toString().c_str(), sizeof(ipAddress));
        Serial.printf("Connected! IP: %s\n", ipAddress);
    } else {
        Serial.println("WiFi connection failed");
    }

    // Audio
    Audio::audio_info_callback = audioCallback;
    audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_MCLK);
    audio.setVolume(vol);
    audio.setConnectionTimeout(5000, 0);

    if (wifiConnected) {
        strlcpy(songTitle, "Connecting...", sizeof(songTitle));
        Serial.printf("Connecting to: %s\n", stationUrls[currentStation]);
        audio.connecttohost(stationUrls[currentStation]);
        delay(200);
        digitalWrite(PA_PIN, HIGH);
    } else {
        strlcpy(songTitle, "WiFi not connected", sizeof(songTitle));
    }

    // Start audio on Core 0 (WiFi core) — rendering stays on Core 1
    xTaskCreatePinnedToCore(audioTask, "audio", 8192, NULL, 2, NULL, 0);

    drawFrame();
}

// ─── Audio task (Core 0) ──────────────────────────────────

static void audioTask(void *param) {
    for (;;) {
        audio.loop();
        vTaskDelay(1);
    }
}

// ─── Loop (Core 1 — rendering + input) ───────────────────

void loop() {
    unsigned long now = millis();

    // Animate visualiser bars
    if (now - lastVisMs > 200) {
        lastVisMs = now;
        if (wifiConnected) {
            for (int i = 0; i < 12; i++) graphBars[i] = random(1, 6);
        }
    }

    // Periodic WiFi status update
    if (now - lastStatusMs > 2000) {
        lastStatusMs = now;
        bool wasConnected = wifiConnected;
        wifiConnected = (WiFi.status() == WL_CONNECTED);
        if (wifiConnected) {
            wifiRssi = WiFi.RSSI();
            if (!wasConnected) {
                strlcpy(ipAddress, WiFi.localIP().toString().c_str(), sizeof(ipAddress));
                Serial.println("WiFi reconnected, resuming stream");
                audio.connecttohost(stationUrls[currentStation]);
                strlcpy(songTitle, "Reconnecting...", sizeof(songTitle));
            }
        } else {
            wifiRssi = 0;
            if (wasConnected) {
                Serial.println("WiFi lost, attempting reconnect");
                WiFi.reconnect();
            }
        }
    }

    handleTouch();
    handleButtons();

    // Render frame at ~15fps (66ms) — never starves audio (separate core)
    if (now - lastFrameMs > 66) {
        lastFrameMs = now;
        songScrollX -= 2;
        int tw = strlen(songTitle) * 7;
        if (songScrollX < -tw) songScrollX = 310;
        drawFrame();
    }

    vTaskDelay(1);
}

// ─── Audio callback ───────────────────────────────────────

static void updateId3SongTitle() {
    char newTitle[128];
    if (id3Title[0] && id3Artist[0]) {
        snprintf(newTitle, sizeof(newTitle), "%s - %s", id3Artist, id3Title);
    } else if (id3Title[0]) {
        strlcpy(newTitle, id3Title, sizeof(newTitle));
    } else if (id3Artist[0]) {
        strlcpy(newTitle, id3Artist, sizeof(newTitle));
    } else {
        return;
    }
    if (strcmp(songTitle, newTitle) != 0) {
        strlcpy(songTitle, newTitle, sizeof(songTitle));
        songScrollX = 310;
    }
}

void audioCallback(Audio::msg_t msg) {
    switch (msg.e) {
        case Audio::evt_info:
            Serial.printf("info: %s\n", msg.msg);
            if (msg.msg && strncmp(msg.msg, "BitRate:", 8) == 0) {
                long br = atol(msg.msg + 8);
                if (br > 0) {
                    bitrate = (br >= 1000) ? br / 1000 : br;
                }
            }
            break;
        case Audio::evt_id3data:
            Serial.printf("id3: %s\n", msg.msg);
            if (msg.msg) {
                if (strncmp(msg.msg, "Title: ", 7) == 0) {
                    strlcpy(id3Title, msg.msg + 7, sizeof(id3Title));
                    updateId3SongTitle();
                } else if (strncmp(msg.msg, "Artist: ", 8) == 0) {
                    strlcpy(id3Artist, msg.msg + 8, sizeof(id3Artist));
                    updateId3SongTitle();
                }
            }
            break;
        case Audio::evt_name:
            Serial.printf("station: %s\n", msg.msg);
            if (songTitle[0] == '\0' || strcmp(songTitle, "Connecting...") == 0 ||
                strcmp(songTitle, "Reconnecting...") == 0) {
                if (strcmp(songTitle, msg.msg) != 0) {
                    strlcpy(songTitle, msg.msg, sizeof(songTitle));
                    songScrollX = 310;
                }
            }
            break;
        case Audio::evt_streamtitle:
            Serial.printf("title: %s\n", msg.msg);
            if (msg.msg && msg.msg[0] != '\0' && strcmp(songTitle, msg.msg) != 0) {
                strlcpy(songTitle, msg.msg, sizeof(songTitle));
                songScrollX = 310;
                id3Artist[0] = '\0';
                id3Title[0] = '\0';
            }
            break;
        case Audio::evt_bitrate:
            Serial.printf("bitrate: %s\n", msg.msg);
            if (msg.msg) {
                long br = atol(msg.msg);
                if (br > 0) {
                    bitrate = (br >= 1000) ? br / 1000 : br;
                }
            }
            break;
        case Audio::evt_eof:
            Serial.println("Stream ended");
            break;
        case Audio::evt_log:
            Serial.printf("log: %s\n", msg.msg);
            break;
        default:
            break;
    }
}
