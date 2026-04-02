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
#include "mqtt.h"

// ─── Globals ──────────────────────────────────────────────

static LGFX       tft;
static LGFX_Sprite canvas(&tft);
static LGFX_Sprite tickerSprite(&tft);  // pushed directly to display (not via canvas)
static LGFX_Sprite vizSprite(&tft);     // visualizer pushed directly to display
static Audio       audio;
static Preferences prefs;

// Defined in i2s_bridge.cpp
extern void initBridgeI2S();
extern void deinitBridgeI2S();
extern void updateBridgeSampleRate(uint32_t rate);
// Station list
#define NUM_STATIONS 10
const char* stationUrls[NUM_STATIONS] = {
    "http://ice1.somafm.com/groovesalad-128-mp3",            // Electronic / Chill
    "http://listen2.myradio24.com/8226",                      // New Age / Enigmatic
    "http://hirschmilch.de:7000/psytrance.mp3",              // Psytrance
    "http://stream.rockantenne.de/rockantenne/stream/mp3",   // Rock
    "http://listen.181fm.com/181-hardrock_128k.mp3",         // Hard Rock
    "http://stream.rockantenne.de/heavy-metal/stream/mp3",   // Metal
    "http://listen.181fm.com/181-power_128k.mp3",            // Pop / Top 40
    "https://live.amperwave.net/manifest/audacy-kroqfmaac-hlsc.m3u8", // Alt Rock (KROQ)
    "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service", // News (BBC)
    "http://npr-ice.streamguys1.com/live.mp3"                // News (NPR)
};
const char* stationNames[NUM_STATIONS] = {
    "Groove Salad",
    "Enigmatic Station 1",
    "Psytrance",
    "Rock Antenne",
    "181 Hard Rock",
    "Heavy Metal",
    "181 Power Hits",
    "KROQ",
    "BBC World News",
    "NPR News"
};

// State (shared between Core 0 audio task and Core 1 render loop)
volatile int    currentStation  = 0;
volatile int    vol             = DEFAULT_VOLUME;
char            songTitle[128]  = "";
volatile long   bitrate         = 0;
volatile bool   wifiConnected   = false;
volatile int    wifiRssi        = 0;
volatile bool   btMode          = false;  // true = BT speaker, false = local
char            id3Artist[64]   = "";
char            id3Title[64]    = "";
char            ipAddress[20]   = "";

// Playback state (for MQTT play/pause/stop commands)
enum PlayState : int { PS_STOPPED = 0, PS_PLAYING = 1, PS_PAUSED = 2 };
volatile int    playState       = PS_STOPPED;

// Ticker sprite dimensions
static constexpr int TICKER_SPRITE_W = 316;
static constexpr int TICKER_SPRITE_H = 22;

// Ticker scroll state
static char   tickerPrevTitle[128] = "";
static int    tickerCursorX        = TICKER_SPRITE_W;
static bool   needsFullRedraw      = true;  // push full canvas when static UI changes

// Screen brightness
static const uint8_t BRIGHTNESS_LEVELS[] = {20, 80, 160, 255};
static const int     NUM_BRIGHTNESS      = 4;
static int           brightnessIdx       = 2;   // index for boot-button cycling
volatile uint8_t     brightness          = 160;  // current level (1-255), shared with MQTT
volatile bool        screenOn            = true;
static const uint8_t DIM_BRIGHTNESS      = 10;  // when stopped
static int           prevPlayState       = PS_STOPPED;

// Timing
static unsigned long lastFrameMs   = 0;
static unsigned long lastStatusMs  = 0;
static unsigned long lastTouchMs   = 0;
static unsigned long lastVisMs     = 0;

// Touch highlight feedback (-1=none, 0=minus, 1=mute, 2=plus, 3=slider, 4=bt)
static int       touchHighlight   = -1;
static unsigned long touchHlMs    = 0;

// Winamp 2 gradient: 6 key colors from base (dark blue-green) to peak (red)
static const uint8_t gradR[] = {0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
static const uint8_t gradG[] = {0x40, 0x80, 0xFF, 0xFF, 0x80, 0x00};
static const uint8_t gradB[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x00};

// Interpolate the Winamp 2 gradient at position t (0.0 = base, 1.0 = peak)
static uint16_t gradientColor565(float t) {
    if (t <= 0.0f) t = 0.0f;
    if (t >= 1.0f) t = 1.0f;
    float pos = t * 5.0f;
    int idx = (int)pos;
    if (idx > 4) idx = 4;
    float f = pos - idx;
    uint8_t r = gradR[idx] + (gradR[idx + 1] - gradR[idx]) * f;
    uint8_t g = gradG[idx] + (gradG[idx + 1] - gradG[idx]) * f;
    uint8_t b = gradB[idx] + (gradB[idx + 1] - gradB[idx]) * f;
    return lgfx::color565(r, g, b);
}

// Visualiser display state
static float    displayBars[VIZ_BANDS]  = {0};    // smoothed bars for rendering
static constexpr int MAX_SEGS = 10;               // segments per bar
static constexpr int SEG_H    = 2;                // segment pixel height
static constexpr int SEG_GAP  = 1;                // gap between segments
static uint16_t segColors[MAX_SEGS];              // pre-computed gradient for spectrum

// UI layout constants
static constexpr int LPANEL_W   = 198;
static constexpr int RPANEL_X   = 202;
static constexpr int RPANEL_W   = 116;
static constexpr int HDR_H      = 10;
static constexpr int STA_Y      = 14;
static constexpr int STA_LINE   = 18;
static constexpr int TICKER_Y   = 220;
static constexpr int TICKER_H   = 18;
static constexpr int TICKER_REGION_Y = 206;   // "NOW PLAYING" label Y
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

// Winamp 2 button palette
static constexpr uint16_t WA_BTN_FACE  = 0xC618; // #C0C0C0 silver
static constexpr uint16_t WA_HIGHLIGHT = 0xFFFF; // white bevel
static constexpr uint16_t WA_SHADOW    = 0x8410; // #808080 shadow
static constexpr uint16_t WA_BORDER    = 0x2104; // dark outline

// Retained ES8311 handle (avoid leak)
static es8311_handle_t esHandle = nullptr;

// Forward declarations
void audioCallback(Audio::msg_t msg);
static void audioTask(void *param);

// Pending station change: set on Core 1, consumed on Core 0
static volatile bool pendingConnect  = false;
// Pending audio commands: set on Core 1, consumed on Core 0
static volatile bool pendingPause    = false;
static volatile bool pendingStop     = false;

static void saveStation() {
    prefs.begin("radio", false);
    prefs.putInt("station", currentStation);
    prefs.putInt("volume", vol);
    prefs.putBool("btMode", btMode);
    prefs.putUChar("brightness", brightness);
    prefs.end();
    needsFullRedraw = true;
}

// Deferred NVS save: mark dirty, flush after idle period in loop()
static bool     nvsDirty   = false;
static unsigned long nvsDirtyMs = 0;

static void markDirty() {
    nvsDirty = true;
    nvsDirtyMs = millis();
    needsFullRedraw = true;
}

static void flushIfDirty() {
    if (nvsDirty && (millis() - nvsDirtyMs > 2000)) {
        nvsDirty = false;
        saveStation();
    }
}

static void loadStation() {
    prefs.begin("radio", true);
    currentStation = prefs.getInt("station", 0);
    vol = prefs.getInt("volume", DEFAULT_VOLUME);
    btMode = prefs.getBool("btMode", false);
    brightness = prefs.getUChar("brightness", 160);
    prefs.end();
    if (currentStation < 0 || currentStation >= NUM_STATIONS) currentStation = 0;
    if (vol < 0 || vol > MAX_VOLUME) vol = DEFAULT_VOLUME;
    if (brightness < 1) brightness = 160;
    // Sync brightnessIdx to closest preset for boot-button cycling
    for (int i = 0; i < NUM_BRIGHTNESS; i++) {
        if (abs((int)BRIGHTNESS_LEVELS[i] - (int)brightness) <
            abs((int)BRIGHTNESS_LEVELS[brightnessIdx] - (int)brightness))
            brightnessIdx = i;
    }
    Serial.printf("Restored station %d, volume %d, btMode %d, brightness %d\n",
                  currentStation, vol, (int)btMode, brightness);
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

    // ── Winamp 2 title bar ──
    {
        const int tm = 4;  // top margin
        const int lm = 6;  // left/right margin
        const int ly1 = tm + 1, ly2 = ly1 + 4, lh = 2;

        // Logo "R" (accent on dark bg)
        canvas.setTextColor(cAccent, cBg);
        canvas.drawString("R", lm, tm, 1);

        // Three dummy buttons (right, accent on dark bg): ○ − ×
        int bx = 320 - lm - 8;
        canvas.drawLine(bx + 2, tm + 2, bx + 5, tm + 5, cAccent); // ×
        canvas.drawLine(bx + 5, tm + 2, bx + 2, tm + 5, cAccent);
        bx -= 9;
        canvas.drawFastHLine(bx + 2, tm + 4, 4, cAccent); // −
        bx -= 9;
        canvas.drawRect(bx + 2, tm + 2, 4, 4, cAccent); // ○

        int linesL = lm + 8 + 4;   // after logo + 4px padding
        int linesR = bx - 2;       // before buttons - 2px margin

        // Title (faux bold) centered
        const char* title = "ESP32 RADIO";
        int titleW = 67;
        int titleX = 160;
        int leftEnd = titleX - titleW / 2 - 4;
        int rightStart = titleX + titleW / 2 + 4;

        // Left grooved lines
        canvas.fillRect(linesL, ly1, leftEnd - linesL, lh, cAccent);
        canvas.fillRect(linesL, ly2, leftEnd - linesL, lh, cAccent);
        // Right grooved lines
        canvas.fillRect(rightStart, ly1, linesR - rightStart, lh, cAccent);
        canvas.fillRect(rightStart, ly2, linesR - rightStart, lh, cAccent);

        // Title text (drawn twice offset for faux bold)
        canvas.setTextColor(cBright, cBg);
        canvas.drawCenterString(title, titleX, tm, 1);
        canvas.drawCenterString(title, titleX + 1, tm, 1);
    }

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

    canvas.setTextColor(cDim, cPanel);
    canvas.drawString("WIFI", RPANEL_X + 6, ry + 4, 1);
    // Signal strength bars (4 ascending bars next to WIFI label)
    {
        int bars = 0;
        if (wifiConnected) {
            if      (wifiRssi >= -50) bars = 4;
            else if (wifiRssi >= -60) bars = 3;
            else if (wifiRssi >= -70) bars = 2;
            else                      bars = 1;
        }
        int bx = RPANEL_X + 32, baseY = ry + 12;
        for (int i = 0; i < 4; i++) {
            int bh = 2 + i * 2;
            canvas.fillRect(bx + i * 3, baseY - bh, 2, bh, (i < bars) ? TFT_GREEN : cDim);
        }
    }
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
    canvas.fillRect(RPANEL_X, vy, RPANEL_W, 52, cPanel);
    canvas.drawRect(RPANEL_X, vy, RPANEL_W, 52, cBorder);

    canvas.setTextColor(cDim, cPanel);
    {
        char volLabel[20];
        snprintf(volLabel, sizeof(volLabel), "VOLUME: %d%%", (vol * 100 + MAX_VOLUME / 2) / MAX_VOLUME);
        canvas.drawCenterString(volLabel, RPANEL_X + RPANEL_W / 2, vy + 3, 1);
    }

    int barW = RPANEL_W - 16;
    int barX = RPANEL_X + 8;
    int barY = vy + 18;
    canvas.fillRoundRect(barX, barY, barW, 4, 2, cBarBg);
    int fillW = (vol * barW) / MAX_VOLUME;
    canvas.fillRoundRect(barX, barY, fillW, 4, 2, gradientColor565((float)vol / MAX_VOLUME));
    int knobX = barX + fillW - 5;
    if (knobX < barX) knobX = barX;
    {
        int kw = 10, kh = 12, ky = barY - 4;
        canvas.fillRect(knobX, ky, kw, kh, WA_BTN_FACE);
        canvas.drawRect(knobX, ky, kw, kh, WA_BORDER);
        canvas.drawFastHLine(knobX + 1, ky + 1, kw - 3, WA_HIGHLIGHT);
        canvas.drawFastVLine(knobX + 1, ky + 1, kh - 3, WA_HIGHLIGHT);
        canvas.drawFastHLine(knobX + 1, ky + kh - 2, kw - 2, WA_SHADOW);
        canvas.drawFastVLine(knobX + kw - 2, ky + 1, kh - 2, WA_SHADOW);
    }

    // Volume buttons: [ - ] [ MUTE ] [ + ] (Winamp 2 style)
    bool muted = (vol == 0);
    bool hlActive = (touchHighlight >= 0 && (millis() - touchHlMs < 200));
    // indicator: -1=none, 0=inactive(gray), 1=active(green)
    auto drawWaBtn = [&](int bx, int by2, int bw, int bh, bool pressed, const char* label, int indicator = -1) {
        canvas.fillRect(bx, by2, bw, bh, WA_BTN_FACE);
        canvas.drawRect(bx, by2, bw, bh, WA_BORDER);
        if (!pressed) {
            canvas.drawFastHLine(bx + 1, by2 + 1, bw - 3, WA_HIGHLIGHT);
            canvas.drawFastVLine(bx + 1, by2 + 1, bh - 3, WA_HIGHLIGHT);
            canvas.drawFastHLine(bx + 1, by2 + bh - 2, bw - 2, WA_SHADOW);
            canvas.drawFastVLine(bx + bw - 2, by2 + 1, bh - 2, WA_SHADOW);
        } else {
            canvas.drawRect(bx + 1, by2 + 1, bw - 2, bh - 2, WA_SHADOW);
        }
        int off = pressed ? 1 : 0;
        canvas.setTextColor(WA_BORDER, WA_BTN_FACE);
        if (indicator >= 0) {
            int ix = bx + 3 + off, iy = by2 + (bh - 5) / 2 + off;
            canvas.fillRect(ix, iy, 5, 5, indicator ? TFT_GREEN : WA_SHADOW);
            canvas.drawRect(ix, iy, 5, 5, WA_BORDER);
            canvas.drawString(label, ix + 6, by2 + (bh / 2) - 3 + off, 1);
        } else {
            canvas.drawCenterString(label, bx + bw / 2 + off, by2 + (bh / 2) - 3 + off, 1);
        }
    };
    {
        int btnY = vy + 34, btnH = 15;
        drawWaBtn(RPANEL_X + 4,  btnY, 34, btnH, (hlActive && touchHighlight == 0), "-");
        drawWaBtn(RPANEL_X + 40, btnY, 36, btnH, (hlActive && touchHighlight == 1), "MUTE", muted ? 1 : 0);
        drawWaBtn(RPANEL_X + 78, btnY, 34, btnH, (hlActive && touchHighlight == 2), "+");
    }

    // ── Right panel: Bitrate ──
    int by = vy + 56;
    canvas.fillRect(RPANEL_X, by, RPANEL_W, 16, cPanel);
    canvas.drawRect(RPANEL_X, by, RPANEL_W, 16, cBorder);
    canvas.setTextColor(TFT_GREEN, cPanel);
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "BITRATE %ldk", bitrate);
        canvas.drawString(buf, RPANEL_X + 6, by + 4, 1);
    }

    // ── Right panel: BT/Speaker toggle (Winamp 2 style) ──
    int bty = by + 18;
    drawWaBtn(RPANEL_X + 4, bty, RPANEL_W - 8, 16,
              (hlActive && touchHighlight == 4),
              "BT SPEAKER", btMode ? 1 : 0);

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
        int segStep = SEG_H + SEG_GAP;
        int baseY = VIZ_H - 3;
        for (int i = 0; i < VIZ_BANDS; i++) {
            int numSegs = (int)(displayBars[i] * (MAX_SEGS / 5.0f) + 0.5f);
            if (numSegs > MAX_SEGS) numSegs = MAX_SEGS;
            int bx = startX + i * (barW + gap);
            for (int s = 0; s < numSegs; s++) {
                int sy = baseY - (s + 1) * segStep + SEG_GAP;
                vizSprite.fillRect(bx, sy, barW, SEG_H, segColors[s]);
            }
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

// GT911 home button — read soft-key flag from status register via I2C.
// LovyanGFX only reads touch coordinates from 0x814E; when the GT911 reports
// a key press (bit 4) instead of a coordinate touch, LovyanGFX sees zero points
// and clears the register, discarding the event. We intercept BEFORE getTouch().
static bool prevHomeBtn = false;
static uint8_t gt911Addr = 0x14;  // resolved in setup(); fallback 0x5D

static bool readGT911HomeButton() {
    uint8_t reg[2] = {0x81, 0x4E};
    uint8_t status = 0;

    if (!lgfx::i2c::transactionWriteRead(0, gt911Addr, reg, 2, &status, 1).has_value())
        return false;

    // Bit 7 = data ready, bit 4 = key/button pressed
    if ((status & 0x80) && (status & 0x10)) {
        // Clear status register so GT911 can report new events
        uint8_t clear[3] = {0x81, 0x4E, 0x00};
        lgfx::i2c::transactionWrite(0, gt911Addr, clear, 3);
        return true;
    }
    return false;
}

static void handleTouch() {
    // Home button (red circle below screen) — must check BEFORE getTouch()
    // because getTouch() clears the GT911 status register, discarding key events.
    bool homeBtn = readGT911HomeButton();
    if (homeBtn && !prevHomeBtn) {
        unsigned long now = millis();
        if (now - lastTouchMs >= 300) {
            lastTouchMs = now;
            Serial.println("Home button pressed");
            if (playState == PS_PLAYING || playState == PS_PAUSED) {
                pendingStop = true;
                playState = PS_STOPPED;
                strlcpy(songTitle, "", sizeof(songTitle));
                id3Artist[0] = '\0'; id3Title[0] = '\0';
                bitrate = 0;
                mqttNotifyStateChange();
                needsFullRedraw = true;
            } else {
                strlcpy(songTitle, "Connecting...", sizeof(songTitle));
                playState = PS_PLAYING;
                pendingConnect = true;
                mqttNotifyStateChange();
            }
        }
    }
    prevHomeBtn = homeBtn;

    lgfx::touch_point_t tp;
    if (!tft.getTouch(&tp)) return;

    unsigned long now = millis();
    if (now - lastTouchMs < 300) return;
    lastTouchMs = now;

    int tx = tp.x, ty = tp.y + 12;  // GT911 Y offset compensation

    // DEBUG: draw touch point directly on display
    if (tx >= 0 && tx < 320 && ty >= 0 && ty < 240) {
        tft.drawCircle(tx, ty, 8, lgfx::color565(180, 180, 180));
        tft.drawCircle(tx, ty, 9, lgfx::color565(180, 180, 180));
    }
    Serial.printf("Touch: x=%d y=%d (raw_y=%d)\n", tx, ty, tp.y);

    // Station selection — left panel
    if (tx < LPANEL_W && ty > STA_Y && ty < STA_Y + NUM_STATIONS * STA_LINE + 10) {
        int idx = (ty - STA_Y - 4) / STA_LINE;
        if (idx >= 0 && idx < NUM_STATIONS && idx != currentStation) {
            currentStation = idx;
            strlcpy(songTitle, "Connecting...", sizeof(songTitle));
            id3Artist[0] = '\0'; id3Title[0] = '\0'; bitrate = 0;
            playState = PS_PLAYING;
            pendingConnect = true;
            Serial.printf("Station: %s\n", stationNames[currentStation]);
            saveStation();
            mqttNotifyStateChange();
        }
    }

    // Volume slider — right panel (vy=122, bar drawn at vy+18)
    // Touch zone: vy+10 to vy+26 (centered on bar/knob area)
    int vy = HDR_H + 4 + 60 + 42;  // 122
    int barX = RPANEL_X + 4;
    int barW = RPANEL_W - 8;
    if (tx >= RPANEL_X && tx <= RPANEL_X + RPANEL_W && ty >= vy + 10 && ty <= vy + 26) {
        int clampedX = tx;
        if (clampedX < barX) clampedX = barX;
        if (clampedX > barX + barW) clampedX = barX + barW;
        int newVol = ((clampedX - barX) * MAX_VOLUME + barW / 2) / barW;
        if (newVol < 0) newVol = 0;
        if (newVol > MAX_VOLUME) newVol = MAX_VOLUME;
        vol = newVol;
        audio.setVolume(vol);
        markDirty();
        mqttNotifyStateChange();
        touchHighlight = 3; touchHlMs = millis();
    }

    // Volume buttons: [ - ] [ MUTE ] [ + ] drawn at vy+34
    // Touch zone: vy+30 to vy+54 (4px dead zone above, generous below)
    if (tx > RPANEL_X && ty >= vy + 30 && ty <= vy + 54) {
        if (tx < RPANEL_X + 39 && vol > 0) {
            vol--;
            audio.setVolume(vol);
            markDirty();
            mqttNotifyStateChange();
            touchHighlight = 0; touchHlMs = millis();
        }
        else if (tx >= RPANEL_X + 39 && tx < RPANEL_X + 77) {
            if (vol > 0) { savedVol = vol; vol = 0; }
            else         { vol = savedVol; }
            audio.setVolume(vol);
            markDirty();
            mqttNotifyStateChange();
            touchHighlight = 1; touchHlMs = millis();
        }
        else if (tx >= RPANEL_X + 77 && vol < MAX_VOLUME) {
            vol++;
            audio.setVolume(vol);
            markDirty();
            mqttNotifyStateChange();
            touchHighlight = 2; touchHlMs = millis();
        }
    }

    // BT/Speaker toggle — below bitrate (by = vy + 56, button at by + 18)
    int by = vy + 56;
    int bty = by + 18;
    if (tx >= RPANEL_X && tx <= RPANEL_X + RPANEL_W && ty >= bty && ty <= bty + 16) {
        if (!btMode) {
            initBridgeI2S();
            btMode = true;
            digitalWrite(PA_PIN, LOW);
        } else {
            btMode = false;
            digitalWrite(PA_PIN, HIGH);
            deinitBridgeI2S();
        }
        markDirty();
        mqttNotifyStateChange();
        touchHighlight = 4; touchHlMs = millis();
        Serial.printf("Output: %s\n", btMode ? "BT Speaker" : "Local Speaker");
    }
}

static bool prevBoot = false;
static bool prevMute = false;
static unsigned long lastMuteMs = 0;

static void handleButtons() {
    // Boot button — cycle screen brightness
    bool boot = (digitalRead(BTN_BOOT) == LOW);
    if (boot && !prevBoot) {
        brightnessIdx = (brightnessIdx + 1) % NUM_BRIGHTNESS;
        brightness = BRIGHTNESS_LEVELS[brightnessIdx];
        screenOn = true;
        tft.setBrightness(brightness);
        Serial.printf("Brightness: %d\n", brightness);
        markDirty();
        mqttNotifyStateChange();
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
        markDirty();
        mqttNotifyStateChange();
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

    // Probe GT911 I2C address (0x14 or 0x5D depending on INT pin state at reset)
    Wire.beginTransmission(0x14);
    if (Wire.endTransmission() == 0) {
        gt911Addr = 0x14;
    } else {
        gt911Addr = 0x5D;
    }
    Serial.printf("GT911 addr: 0x%02X\n", gt911Addr);

    pinMode(BTN_BOOT, INPUT_PULLUP);
    pinMode(BTN_MUTE, INPUT_PULLDOWN);

    // Keep PA off until audio is ready to avoid noise
    pinMode(PA_PIN, OUTPUT);
    digitalWrite(PA_PIN, LOW);

    initCodec();

    // Display
    tft.init();
    tft.setBrightness(brightness);
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
    for (int s = 0; s < MAX_SEGS; s++)
        segColors[s] = gradientColor565((float)s / (MAX_SEGS - 1));

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

    // I2S1 bridge to WROOM-32D A2DP bridge (lazy — only when BT active)
    if (btMode) initBridgeI2S();

    // FFT init
    spectrumInit();

    if (wifiConnected) {
        strlcpy(songTitle, "Connecting...", sizeof(songTitle));
        Serial.printf("Connecting to: %s\n", stationUrls[currentStation]);
        audio.connecttohost(stationUrls[currentStation]);
        playState = PS_PLAYING;
        delay(200);
        if (!btMode) digitalWrite(PA_PIN, HIGH);  // only enable PA for local speaker
    } else {
        strlcpy(songTitle, "WiFi not connected", sizeof(songTitle));
    }

    // MQTT (after WiFi is connected)
    mqttInit();

    // Start audio on Core 0 (WiFi core) — rendering stays on Core 1
    xTaskCreatePinnedToCore(audioTask, "audio", 12288, NULL, 2, NULL, 0);

    drawFrame();
    drawVisualizer();
}

// ─── Audio task (Core 0) ──────────────────────────────────

static void audioTask(void *param) {
    for (;;) {
        if (pendingStop) {
            pendingStop = false;
            audio.stopSong();
        }
        if (pendingPause) {
            pendingPause = false;
            audio.pauseResume();
        }
        if (pendingConnect) {
            pendingConnect = false;
            audio.connecttohost(stationUrls[currentStation]);
        }
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
                pendingConnect = true;
                playState = PS_PLAYING;
                strlcpy(songTitle, "Reconnecting...", sizeof(songTitle));
                mqttNotifyStateChange();
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
    flushIfDirty();

    // Auto-dim screen when stopped, restore when playing
    int ps = playState;  // snapshot volatile once to avoid TOCTOU
    if (ps != prevPlayState) {
        if (screenOn) {
            if (ps == PS_STOPPED)
                tft.setBrightness(DIM_BRIGHTNESS);
            else if (prevPlayState == PS_STOPPED)
                tft.setBrightness(brightness);
        }
        prevPlayState = ps;
    }

    // MQTT: process broker messages and publish state if dirty
    mqttLoop();

    // Process MQTT commands (same pattern as touch/button handlers)
    if (mqttCmdPending) {
        mqttCmdPending = false;
        MqttCmd cmd;
        cmd.type    = mqttCmd.type;
        cmd.intVal  = mqttCmd.intVal;
        cmd.boolVal = mqttCmd.boolVal;

        switch (cmd.type) {
            case MQTT_CMD_VOLUME:
                vol = cmd.intVal;
                if (vol < 0) vol = 0;
                if (vol > MAX_VOLUME) vol = MAX_VOLUME;
                audio.setVolume(vol);
                markDirty();
                mqttNotifyStateChange();
                break;
            case MQTT_CMD_MUTE:
                if (cmd.boolVal && vol > 0) { savedVol = vol; vol = 0; }
                else if (!cmd.boolVal && vol == 0) { vol = savedVol; }
                audio.setVolume(vol);
                markDirty();
                mqttNotifyStateChange();
                break;
            case MQTT_CMD_SOURCE:
                if (cmd.intVal >= 0 && cmd.intVal < NUM_STATIONS && cmd.intVal != currentStation) {
                    currentStation = cmd.intVal;
                    strlcpy(songTitle, "Connecting...", sizeof(songTitle));
                    id3Artist[0] = '\0'; id3Title[0] = '\0'; bitrate = 0;
                    playState = PS_PLAYING;
                    pendingConnect = true;
                    markDirty();
                    mqttNotifyStateChange();
                }
                break;
            case MQTT_CMD_NEXT:
                currentStation = (currentStation + 1) % NUM_STATIONS;
                strlcpy(songTitle, "Connecting...", sizeof(songTitle));
                id3Artist[0] = '\0'; id3Title[0] = '\0'; bitrate = 0;
                playState = PS_PLAYING;
                pendingConnect = true;
                markDirty();
                mqttNotifyStateChange();
                break;
            case MQTT_CMD_PREV:
                currentStation = (currentStation - 1 + NUM_STATIONS) % NUM_STATIONS;
                strlcpy(songTitle, "Connecting...", sizeof(songTitle));
                id3Artist[0] = '\0'; id3Title[0] = '\0'; bitrate = 0;
                playState = PS_PLAYING;
                pendingConnect = true;
                markDirty();
                mqttNotifyStateChange();
                break;
            case MQTT_CMD_PLAY:
                if (playState == PS_PAUSED) {
                    pendingPause = true;  // pauseResume() toggle on Core 0
                    playState = PS_PLAYING;
                } else if (playState == PS_STOPPED) {
                    strlcpy(songTitle, "Connecting...", sizeof(songTitle));
                    playState = PS_PLAYING;
                    pendingConnect = true;
                }
                mqttNotifyStateChange();
                break;
            case MQTT_CMD_PAUSE:
                if (playState == PS_PLAYING) {
                    pendingPause = true;  // pauseResume() toggle on Core 0
                    playState = PS_PAUSED;
                    mqttNotifyStateChange();
                }
                break;
            case MQTT_CMD_STOP:
                if (playState != PS_STOPPED) {
                    pendingStop = true;  // stopSong() on Core 0
                    playState = PS_STOPPED;
                    strlcpy(songTitle, "", sizeof(songTitle));
                    bitrate = 0;
                    mqttNotifyStateChange();
                    needsFullRedraw = true;
                }
                break;
            case MQTT_CMD_BRIGHTNESS:
                brightness = cmd.intVal;
                screenOn = true;
                tft.setBrightness(brightness);
                markDirty();
                mqttNotifyStateChange();
                break;
            case MQTT_CMD_SCREEN:
                screenOn = cmd.boolVal;
                tft.setBrightness(screenOn ? brightness : 0);
                mqttNotifyStateChange();
                break;
            case MQTT_CMD_NONE:
                break;
        }
    }

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
        mqttNotifyStateChange();
    }
}

void audioCallback(Audio::msg_t msg) {
    switch (msg.e) {
        case Audio::evt_info:
            Serial.printf("info: %s\n", msg.msg);
            if (msg.msg && strncmp(msg.msg, "SampleRate (Hz):", 16) == 0) {
                uint32_t sr = atol(msg.msg + 16);
                if (sr > 0) updateBridgeSampleRate(sr);
            }
            if (msg.msg && strncmp(msg.msg, "BitRate:", 8) == 0) {
                long br = atol(msg.msg + 8);
                if (br > 0) {
                    bitrate = (br >= 1000) ? br / 1000 : br;
                    mqttNotifyStateChange();
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
            playState = PS_PLAYING;
            mqttNotifyStateChange();
            if (songTitle[0] == '\0' || strcmp(songTitle, "Connecting...") == 0 ||
                strcmp(songTitle, "Reconnecting...") == 0) {
                if (strcmp(songTitle, msg.msg) != 0) {
                    strlcpy(songTitle, msg.msg, sizeof(songTitle));
                    mqttNotifyStateChange();
                }
            }
            break;
        case Audio::evt_streamtitle:
            Serial.printf("title: %s\n", msg.msg);
            if (msg.msg && msg.msg[0] != '\0' && strcmp(songTitle, msg.msg) != 0) {
                strlcpy(songTitle, msg.msg, sizeof(songTitle));
                id3Artist[0] = '\0';
                id3Title[0] = '\0';
                mqttNotifyStateChange();
            }
            break;
        case Audio::evt_bitrate:
            Serial.printf("bitrate: %s\n", msg.msg);
            if (msg.msg) {
                long br = atol(msg.msg);
                if (br > 0) {
                    bitrate = (br >= 1000) ? br / 1000 : br;
                    mqttNotifyStateChange();
                }
            }
            break;
        case Audio::evt_eof:
            Serial.println("Stream ended");
            playState = PS_STOPPED;
            mqttNotifyStateChange();
            break;
        case Audio::evt_log:
            Serial.printf("log: %s\n", msg.msg);
            break;
        default:
            break;
    }
}
