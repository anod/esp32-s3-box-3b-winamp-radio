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
#include "spectrum.h"

// ─── Globals ──────────────────────────────────────────────

static LGFX       tft;
static LGFX_Sprite canvas(&tft);
static LGFX_Sprite tickerSprite(&tft);  // pushed directly to display (not via canvas)
static LGFX_Sprite vizSprite(&tft);     // visualizer pushed directly to display
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
static char            id3Artist[64]   = "";
static char            id3Title[64]    = "";
static char            ipAddress[20]   = "";

// Ticker sprite dimensions
static constexpr int TICKER_SPRITE_W = 316;
static constexpr int TICKER_SPRITE_H = 22;

// Ticker scroll state
static char   tickerPrevTitle[128] = "";
static int    tickerCursorX        = TICKER_SPRITE_W;
static bool   needsFullRedraw      = true;  // push full canvas when static UI changes

// Timing
static unsigned long lastFrameMs   = 0;
static unsigned long lastStatusMs  = 0;
static unsigned long lastTouchMs   = 0;
static unsigned long lastVisMs     = 0;

// Touch highlight feedback (-1=none, 0=minus, 1=mute, 2=plus, 3=slider)
static int       touchHighlight   = -1;
static unsigned long touchHlMs    = 0;

// Visualiser display state
static float    displayBars[VIZ_BANDS]  = {0};    // smoothed bars for rendering
static uint16_t barColors[6];            // pre-computed gradient

// UI layout constants
static constexpr int LPANEL_W   = 198;
static constexpr int RPANEL_X   = 202;
static constexpr int RPANEL_W   = 116;
static constexpr int HDR_H      = 16;
static constexpr int STA_Y      = 20;
static constexpr int STA_LINE   = 18;
static constexpr int TICKER_Y   = 220;
static constexpr int TICKER_H   = 18;
static constexpr int TICKER_REGION_Y = 210;   // "NOW PLAYING" label Y
static constexpr int TICKER_BOX_Y   = 218;   // top of ticker box (canvas stops here)
static constexpr int CANVAS_H       = 218;   // canvas height (above ticker box)

// Visualizer sprite position and size (drawn directly to display)
static constexpr int VIZ_X = RPANEL_X;
static constexpr int VIZ_Y = HDR_H + 4 + 60;  // ry + 60
static constexpr int VIZ_W = RPANEL_W;
static constexpr int VIZ_H = 38;

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
    needsFullRedraw = true;
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
    canvas.fillRect(2, HDR_H + 4, LPANEL_W - 4, NUM_STATIONS * STA_LINE + 8, cPanel);
    canvas.drawRect(2, HDR_H + 4, LPANEL_W - 4, NUM_STATIONS * STA_LINE + 8, cBorder);
    canvas.fillRect(4, HDR_H + 1, LPANEL_W - 8, 2, cAccent);

    // Header labels — centered within their panel
    canvas.setTextColor(cBright, cBg);
    canvas.drawCenterString("STATIONS", 2 + (LPANEL_W - 4) / 2, 2, 2);
    canvas.drawCenterString("INTERNET RADIO", RPANEL_X + RPANEL_W / 2, 2, 2);

    for (int i = 0; i < NUM_STATIONS; i++) {
        int y = STA_Y + 6 + i * STA_LINE;
        if (i == currentStation) {
            canvas.fillRect(4, y - 1, LPANEL_W - 8, STA_LINE - 2, cStationHl);
            canvas.setTextColor(TFT_GREEN, cStationHl);
            canvas.drawString(">", 8, y, 2);
        } else {
            canvas.setTextColor(cStationDim, cPanel);
        }
        canvas.drawString(stationNames[i], 24, y, 2);
    }

    // Divider between panels
    canvas.fillRect(LPANEL_W, HDR_H + 4, 3, NUM_STATIONS * STA_LINE + 8, cBorder);

    // ── Right panel: WiFi ──
    int ry = HDR_H + 4;
    canvas.fillRect(RPANEL_X, ry, RPANEL_W, 56, cPanel);
    canvas.drawRect(RPANEL_X, ry, RPANEL_W, 56, cBorder);
    canvas.fillRect(RPANEL_X + 2, HDR_H + 1, RPANEL_W - 4, 2, cAccent);

    canvas.setTextColor(cDim, cPanel);
    canvas.drawString("WIFI", RPANEL_X + 6, ry + 4, 1);
    canvas.setTextColor(wifiConnected ? TFT_GREEN : TFT_RED, cPanel);
    canvas.drawString(wifiConnected ? "CONNECTED" : "OFFLINE", RPANEL_X + 6, ry + 16, 1);
    canvas.setTextColor(cBright, cPanel);
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "RSSI:%d", wifiRssi);
        canvas.drawString(buf, RPANEL_X + 6, ry + 28, 1);
    }
    if (wifiConnected) {
        canvas.setTextColor(cDim, cPanel);
        canvas.drawString(ipAddress, RPANEL_X + 6, ry + 42, 1);
    }

    // ── Right panel: Volume ──
    int gy = ry + 60;
    int vy = gy + 42;
    canvas.fillRect(RPANEL_X, vy, RPANEL_W, 44, cPanel);
    canvas.drawRect(RPANEL_X, vy, RPANEL_W, 44, cBorder);

    canvas.setTextColor(cDim, cPanel);
    {
        char volLabel[16];
        snprintf(volLabel, sizeof(volLabel), "VOLUME %d", vol);
        canvas.drawCenterString(volLabel, RPANEL_X + RPANEL_W / 2, vy + 3, 1);
    }

    int barW = RPANEL_W - 16;
    int barX = RPANEL_X + 8;
    int barY = vy + 16;
    canvas.fillRoundRect(barX, barY, barW, 4, 2, cBarBg);
    int fillW = (vol * barW) / MAX_VOLUME;
    canvas.fillRoundRect(barX, barY, fillW, 4, 2, TFT_YELLOW);
    int knobX = barX + fillW - 4;
    if (knobX < barX) knobX = barX;
    canvas.fillRoundRect(knobX, barY - 4, 8, 12, 3, cBright);

    // Volume buttons: [ - ] [ MUTE ] [ + ]
    bool muted = (vol == 0);
    bool hlActive = (touchHighlight >= 0 && (millis() - touchHlMs < 200));
    uint16_t minusCol = (hlActive && touchHighlight == 0) ? TFT_WHITE : cBtnCol;
    uint16_t muteCol  = (hlActive && touchHighlight == 1) ? TFT_WHITE : (muted ? TFT_RED : cBtnCol);
    uint16_t plusCol   = (hlActive && touchHighlight == 2) ? TFT_WHITE : cBtnCol;
    canvas.fillRoundRect(RPANEL_X + 4,  vy + 26, 33, 15, 3, minusCol);
    canvas.fillRoundRect(RPANEL_X + 41, vy + 26, 33, 15, 3, muteCol);
    canvas.fillRoundRect(RPANEL_X + 78, vy + 26, 33, 15, 3, plusCol);
    canvas.setTextColor(cBright, minusCol);
    canvas.drawCenterString("-",    RPANEL_X + 20,  vy + 30, 1);
    canvas.setTextColor(cBright, muteCol);
    canvas.drawCenterString("MUTE", RPANEL_X + 57,  vy + 30, 1);
    canvas.setTextColor(cBright, plusCol);
    canvas.drawCenterString("+",    RPANEL_X + 94,  vy + 30, 1);

    // ── Right panel: Bitrate ──
    int by = vy + 48;
    canvas.fillRect(RPANEL_X, by, RPANEL_W, 16, cPanel);
    canvas.drawRect(RPANEL_X, by, RPANEL_W, 16, cBorder);
    canvas.setTextColor(TFT_GREEN, cPanel);
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "BITRATE %ldk", bitrate);
        canvas.drawString(buf, RPANEL_X + 6, by + 4, 1);
    }

    // ── "NOW PLAYING" label (inside canvas, above ticker box) ──
    canvas.setTextColor(cDim, cBg);
    canvas.drawString("NOW PLAYING", 6, TICKER_REGION_Y, 1);

    // Outer frame (canvas height only, bottom border at y=217)
    canvas.drawRect(0, 0, 320, CANVAS_H, cBorder);

    canvas.pushSprite(0, 0);
}

// ─── Visualizer (pushed directly to display at its own cadence) ──

static void drawVisualizer() {
    // Smooth decay: read spectrum bands and ease toward target
    for (int i = 0; i < VIZ_BANDS; i++) {
        float target = specBands[i];
        // log10 scaling: subtract 2.0 so that ~100 maps to 0, ~500K maps to ~3.7
        // Then scale by 1.3 to spread across 0-5 range
        target = (target > 100.0f) ? (log10f(target) - 2.0f) * 1.3f : 0.0f;
        if (target > 5.0f) target = 5.0f;
        if (target < 0.0f) target = 0.0f;
        // Fast attack, slow decay
        if (target > displayBars[i])
            displayBars[i] += (target - displayBars[i]) * 0.6f;
        else
            displayBars[i] += (target - displayBars[i]) * 0.15f;
    }

    vizSprite.fillSprite(cPanel);
    vizSprite.drawRect(0, 0, VIZ_W, VIZ_H, cBorder);

    if (wifiConnected) {
        int barW = 5, gap = 2;
        int totalW = VIZ_BANDS * barW + (VIZ_BANDS - 1) * gap;
        int startX = (VIZ_W - totalW) / 2;
        for (int i = 0; i < VIZ_BANDS; i++) {
            int level = (int)(displayBars[i] + 0.5f);
            if (level < 0) level = 0;
            if (level > 5) level = 5;
            int bh = level * 5 + 2;
            int bx = startX + i * (barW + gap);
            int by = VIZ_H - 3 - bh;
            vizSprite.fillRect(bx, by, barW, bh, barColors[level]);
        }
    }

    vizSprite.pushSprite(VIZ_X, VIZ_Y);
}

// ─── Ticker scroll (using sprite.scroll(), pushed directly to display) ──


static void scrollTicker() {
    static int textW = 0;

    // Snapshot to avoid cross-core race with audioCallback on Core 0
    char titleSnap[128];
    strlcpy(titleSnap, songTitle, sizeof(titleSnap));

    // Detect title change — restart from right edge
    if (strcmp(tickerPrevTitle, titleSnap) != 0) {
        strlcpy(tickerPrevTitle, titleSnap, sizeof(tickerPrevTitle));
        textW = tickerSprite.textWidth(titleSnap, &fonts::Font2);
        tickerCursorX = TICKER_SPRITE_W;
    }

    tickerSprite.fillSprite(cPanel);
    tickerSprite.drawRect(0, 0, TICKER_SPRITE_W, TICKER_SPRITE_H, cBorder);
    tickerSprite.setTextColor(TFT_GREEN, cPanel);
    tickerSprite.drawString(titleSnap, tickerCursorX, 3, 2);
    tickerCursorX -= 1;

    // When text fully scrolled off left, restart from right
    if (tickerCursorX < -textW) {
        tickerCursorX = TICKER_SPRITE_W;
    }

    tickerSprite.pushSprite(2, TICKER_BOX_Y);
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
            audio.connecttohost(stationUrls[currentStation]);
            Serial.printf("Station: %s\n", stationNames[currentStation]);
            saveStation();
        }
    }

    // Volume slider — right panel (vy=122, bar drawn at vy+16)
    // Touch zone: vy+4 to vy+20 (centered on bar, avoids button area)
    int vy = HDR_H + 4 + 60 + 42;  // 122
    int barX = RPANEL_X + 4;
    int barW = RPANEL_W - 8;
    if (tx >= RPANEL_X && tx <= RPANEL_X + RPANEL_W && ty >= vy + 4 && ty <= vy + 20) {
        int clampedX = tx;
        if (clampedX < barX) clampedX = barX;
        if (clampedX > barX + barW) clampedX = barX + barW;
        int newVol = ((clampedX - barX) * MAX_VOLUME + barW / 2) / barW;
        if (newVol < 0) newVol = 0;
        if (newVol > MAX_VOLUME) newVol = MAX_VOLUME;
        vol = newVol;
        audio.setVolume(vol);
        saveStation();
        touchHighlight = 3; touchHlMs = millis();
    }

    // Volume buttons: [ - ] [ MUTE ] [ + ] drawn at vy+26
    // Touch zone: vy+20 to vy+48 (generous below)
    if (tx > RPANEL_X && ty >= vy + 20 && ty <= vy + 48) {
        if (tx < RPANEL_X + 37 && vol > 0) {
            vol--;
            audio.setVolume(vol);
            saveStation();
            touchHighlight = 0; touchHlMs = millis();
        }
        else if (tx >= RPANEL_X + 38 && tx < RPANEL_X + 76) {
            if (vol > 0) { savedVol = vol; vol = 0; }
            else         { vol = savedVol; }
            audio.setVolume(vol);
            saveStation();
            touchHighlight = 1; touchHlMs = millis();
        }
        else if (tx >= RPANEL_X + 76 && vol < MAX_VOLUME) {
            vol++;
            audio.setVolume(vol);
            saveStation();
            touchHighlight = 2; touchHlMs = millis();
        }
    }
}

static bool prevBoot = false;
static bool prevMute = false;
static unsigned long lastMuteMs = 0;

static void handleButtons() {
    // Boot button — next station
    bool boot = (digitalRead(BTN_BOOT) == LOW);
    if (boot && !prevBoot) {
        currentStation = (currentStation + 1) % NUM_STATIONS;
        strlcpy(songTitle, "Connecting...", sizeof(songTitle));
        id3Artist[0] = '\0'; id3Title[0] = '\0'; bitrate = 0;
        audio.connecttohost(stationUrls[currentStation]);
        saveStation();
    }
    prevBoot = boot;

    // Hardware mute button (toggle switch on top) — trigger on any edge
    bool mute = (digitalRead(BTN_MUTE) == HIGH);
    unsigned long now = millis();
    if (mute != prevMute && (now - lastMuteMs > 300)) {
        lastMuteMs = now;
        if (vol > 0) { savedVol = vol; vol = 0; }
        else         { vol = savedVol; }
        audio.setVolume(vol);
        saveStation();
    }
    prevMute = mute;
}

// ─── Setup ────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== ESP32-S3-BOX-3 Internet Radio ===");

    loadStation();

    // I2C for ES8311 codec (port 0, shared with touch)
    Wire.begin(I2C_SDA, I2C_SCL);

    pinMode(BTN_BOOT, INPUT_PULLUP);
    pinMode(BTN_MUTE, INPUT_PULLDOWN);

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
    canvas.createSprite(320, CANVAS_H);  // only above ticker box

    // Ticker sprite: covers rows 218-239, pushed directly to display
    tickerSprite.setColorDepth(16);
    tickerSprite.setPsram(false);
    tickerSprite.createSprite(TICKER_SPRITE_W, TICKER_SPRITE_H);
    tickerSprite.setTextFont(2);

    // Visualizer sprite: pushed directly to display
    vizSprite.setColorDepth(16);
    vizSprite.setPsram(false);
    vizSprite.createSprite(VIZ_W, VIZ_H);

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

    // FFT init
    spectrumInit();

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
    xTaskCreatePinnedToCore(audioTask, "audio", 12288, NULL, 2, NULL, 0);

    drawFrame();
    drawVisualizer();
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

    // Update visualiser at ~50fps (20ms) — smooth decay driven by real spectrum data
    if (now - lastVisMs > 20) {
        lastVisMs = now;
        drawVisualizer();
    }

    // Periodic WiFi status update
    if (now - lastStatusMs > 2000) {
        lastStatusMs = now;
        needsFullRedraw = true;
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

    // Clear touch highlight after 200ms
    if (touchHighlight >= 0 && now - touchHlMs >= 200) {
        touchHighlight = -1;
        needsFullRedraw = true;
    }

    // Render at ~60fps (16ms)
    if (now - lastFrameMs > 16) {
        lastFrameMs = now;

        // Full canvas push only when static UI changed
        if (needsFullRedraw) {
            needsFullRedraw = false;
            drawFrame();
        }

        // Ticker scrolls every frame — small fast push directly to display
        scrollTicker();
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
                }
            }
            break;
        case Audio::evt_streamtitle:
            Serial.printf("title: %s\n", msg.msg);
            if (msg.msg && msg.msg[0] != '\0' && strcmp(songTitle, msg.msg) != 0) {
                strlcpy(songTitle, msg.msg, sizeof(songTitle));
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
