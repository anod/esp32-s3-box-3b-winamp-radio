// ============================================================
// ESP32-S3-BOX Internet Radio
// Based on VolosR/WaveshareRadioStream, adapted for ESP32-S3-BOX (v1)
// Display: 320x240 ILI9342C | Audio: ES8311 | Touch: TT21100
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <Audio.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>
#include "config.h"
#include "pins.h"
#include "es8311.h"

// ─── Display configuration for ESP32-S3-BOX (v1) ─────────

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9342  _panel_instance;
    lgfx::Bus_SPI        _bus_instance;
    lgfx::Light_PWM      _light_instance;
    lgfx::Touch_TT21xxx  _touch_instance;

public:
    LGFX(void) {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host    = SPI3_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = LCD_SCK;
            cfg.pin_mosi    = LCD_MOSI;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = LCD_DC;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs          = LCD_CS;
            cfg.pin_rst         = LCD_RST;
            cfg.pin_busy        = -1;
            cfg.memory_width    = 320;
            cfg.memory_height   = 240;
            cfg.panel_width     = 320;
            cfg.panel_height    = 240;
            cfg.offset_x        = 0;
            cfg.offset_y        = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable        = true;
            cfg.invert          = true;
            cfg.rgb_order       = false;
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = false;
            _panel_instance.config(cfg);
        }
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl      = LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }
        {
            auto cfg = _touch_instance.config();
            cfg.x_min           = 0;
            cfg.x_max           = 319;
            cfg.y_min           = 0;
            cfg.y_max           = 239;
            cfg.pin_int         = TOUCH_INT;
            cfg.bus_shared      = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port        = I2C_NUM_1;
            cfg.i2c_addr        = 0x24;
            cfg.pin_sda         = I2C_SDA;
            cfg.pin_scl         = I2C_SCL;
            cfg.freq            = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
        setPanel(&_panel_instance);
    }
};

// ─── Globals ──────────────────────────────────────────────

static LGFX       tft;
static LGFX_Sprite canvas(&tft);
static LGFX_Sprite ticker(&tft);
static Audio       audio;

// Station list
#define NUM_STATIONS 8
static const char* stationUrls[NUM_STATIONS] = {
    "http://stream.live.vc.bbcmedia.co.uk/bbc_radio_two",
    "https://discodiamond.radioca.st/autodj",
    "http://sc6.radiocaroline.net:8040/stream",
    "https://listen.radioking.com/radio/175279/stream/216784",
    "https://club-high.rautemusik.fm/;",
    "http://greece-media.monroe.edu/wgmc.mp3",
    "https://audio.radio-banovina.hr:9998/;",
    "http://icecast.vrtcdn.be/stubru-high.mp3"
};
static const char* stationNames[NUM_STATIONS] = {
    "BBC Radio 2",
    "Disco Diamond",
    "Radio Caroline",
    "RadioKing",
    "RauteMusik Club",
    "WGMC Jazz",
    "Radio Banovina",
    "Studio Brussel"
};

// State
static int    currentStation  = 0;
static int    vol             = DEFAULT_VOLUME;
static String songTitle       = "";
static long   bitrate         = 0;
static bool   wifiConnected   = false;
static int    wifiRssi        = 0;
static int    songScrollX     = 320;
static bool   needsRedraw     = true;

// Timing
static unsigned long lastStatusMs  = 0;
static unsigned long lastTickerMs  = 0;
static unsigned long lastTouchMs   = 0;

// Visualiser bars
static int graphBars[14] = {0};

// UI layout constants
static constexpr int LPANEL_W   = 198;
static constexpr int RPANEL_X   = 202;
static constexpr int RPANEL_W   = 116;
static constexpr int HDR_H      = 16;
static constexpr int STA_Y      = 20;
static constexpr int STA_LINE   = 21;
static constexpr int TICKER_Y   = 220;
static constexpr int TICKER_H   = 18;

// Theme colours (set once in setup)
static uint16_t cBg, cPanel, cBorder, cAccent, cDim, cBright;

// ─── ES8311 codec init ───────────────────────────────────

static bool initCodec() {
    es8311_handle_t es = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!es) { Serial.println("ES8311 create failed"); return false; }

    const es8311_clock_config_t clk = {
        .mclk_inverted    = false,
        .sclk_inverted    = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency   = ES8311_MCLK_FREQ_HZ,
        .sample_frequency = ES8311_SAMPLE_RATE
    };

    if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        Serial.println("ES8311 init failed");
        return false;
    }
    es8311_sample_frequency_config(es, ES8311_MCLK_FREQ_HZ, ES8311_SAMPLE_RATE);
    es8311_voice_volume_set(es, ES8311_VOICE_VOLUME, NULL);
    es8311_microphone_config(es, false);
    Serial.println("ES8311 codec OK");
    return true;
}

// ─── Drawing ──────────────────────────────────────────────

static void drawUI() {
    canvas.fillSprite(cBg);

    // ── Left panel: station list ──
    canvas.fillRect(2, HDR_H + 2, LPANEL_W - 4, NUM_STATIONS * STA_LINE + 8, cPanel);
    canvas.drawRect(2, HDR_H + 2, LPANEL_W - 4, NUM_STATIONS * STA_LINE + 8, cBorder);
    canvas.fillRect(4, HDR_H, LPANEL_W - 8, 2, cAccent);

    // Header labels
    canvas.setTextColor(cBright, cBg);
    canvas.drawString("STATIONS", 56, 2, 2);
    canvas.drawString("INTERNET RADIO", RPANEL_X, 2, 2);

    // Station entries
    for (int i = 0; i < NUM_STATIONS; i++) {
        int y = STA_Y + 4 + i * STA_LINE;
        if (i == currentStation) {
            uint16_t hl = canvas.color565(0, 40, 0);
            canvas.fillRect(4, y - 1, LPANEL_W - 8, STA_LINE - 2, hl);
            canvas.setTextColor(TFT_GREEN, hl);
            canvas.drawString(">", 8, y, 2);
        } else {
            canvas.setTextColor(canvas.color565(0, 140, 0), cPanel);
        }
        canvas.drawString(stationNames[i], 24, y, 2);
    }

    // Divider between panels
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
    canvas.drawString("RSSI:" + String(wifiRssi), RPANEL_X + 4, ry + 28, 1);
    if (wifiConnected) {
        canvas.setTextColor(cDim, cPanel);
        canvas.drawString(WiFi.localIP().toString(), RPANEL_X + 4, ry + 40, 1);
    }

    // ── Right panel: Visualiser ──
    int gy = ry + 60;
    canvas.fillRect(RPANEL_X, gy, RPANEL_W, 38, cPanel);
    canvas.drawRect(RPANEL_X, gy, RPANEL_W, 38, cBorder);
    for (int i = 0; i < 12; i++) {
        if (wifiConnected) graphBars[i] = random(1, 6);
        for (int j = 0; j < graphBars[i]; j++) {
            uint16_t bc = canvas.color565(0, 100 + j * 30, 50 + j * 20);
            canvas.fillRect(RPANEL_X + 6 + i * 9, gy + 30 - j * 6, 7, 5, bc);
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
    canvas.fillRoundRect(barX, barY, barW, 4, 2, canvas.color565(60, 60, 60));
    int fillW = (vol * barW) / MAX_VOLUME;
    canvas.fillRoundRect(barX, barY, fillW, 4, 2, TFT_YELLOW);
    int knobX = barX + fillW - 4;
    if (knobX < barX) knobX = barX;
    canvas.fillRoundRect(knobX, barY - 4, 8, 12, 3, cBright);

    // +/- buttons
    uint16_t btnCol = canvas.color565(80, 80, 100);
    canvas.fillRoundRect(RPANEL_X + 6,  vy + 24, 46, 15, 3, btnCol);
    canvas.fillRoundRect(RPANEL_X + 62, vy + 24, 46, 15, 3, btnCol);
    canvas.setTextColor(cBright, btnCol);
    canvas.drawString("VOL -", RPANEL_X + 10,  vy + 27, 1);
    canvas.drawString("VOL +", RPANEL_X + 66, vy + 27, 1);

    // ── Right panel: Bitrate ──
    int by = vy + 46;
    canvas.fillRect(RPANEL_X, by, RPANEL_W, 16, cPanel);
    canvas.drawRect(RPANEL_X, by, RPANEL_W, 16, cBorder);
    canvas.setTextColor(TFT_GREEN, cPanel);
    canvas.drawString("BITRATE " + String(bitrate) + "k", RPANEL_X + 4, by + 3, 1);

    // ── Song title area ──
    canvas.fillRect(2, 200, 316, 14, cBg);
    canvas.setTextColor(cDim, cBg);
    canvas.drawString("NOW PLAYING", 6, 202, 1);

    canvas.fillRect(2, TICKER_Y - 4, 316, TICKER_H + 6, cPanel);
    canvas.drawRect(2, TICKER_Y - 4, 316, TICKER_H + 6, cBorder);

    // Outer frame
    canvas.drawRect(0, 0, 320, 240, cBorder);

    canvas.pushSprite(0, 0);
    needsRedraw = false;
}

static void drawTicker() {
    songScrollX -= 2;
    int tw = songTitle.length() * 7;
    if (songScrollX < -tw) songScrollX = 310;

    ticker.fillSprite(cPanel);
    ticker.setTextColor(cBright, cPanel);
    ticker.drawString(songTitle, songScrollX, 2, 2);
    ticker.pushSprite(4, TICKER_Y - 2);
}

// ─── Input handling ───────────────────────────────────────

static void handleTouch() {
    lgfx::touch_point_t tp;
    if (!tft.getTouch(&tp)) return;

    unsigned long now = millis();
    if (now - lastTouchMs < 300) return;
    lastTouchMs = now;

    int tx = tp.x, ty = tp.y;

    // Station selection
    if (tx < LPANEL_W && ty > STA_Y && ty < STA_Y + NUM_STATIONS * STA_LINE) {
        int idx = (ty - STA_Y - 4) / STA_LINE;
        if (idx >= 0 && idx < NUM_STATIONS && idx != currentStation) {
            currentStation = idx;
            songTitle  = "Connecting...";
            songScrollX = 10;
            audio.connecttohost(stationUrls[currentStation]);
            needsRedraw = true;
            Serial.printf("Station: %s\n", stationNames[currentStation]);
        }
    }

    // Volume buttons
    int vy = HDR_H + 2 + 56 + 38 + 42 + 24;   // matches VOL button Y
    if (ty > vy && ty < vy + 15) {
        if (tx > RPANEL_X + 6 && tx < RPANEL_X + 52 && vol > 0) {
            vol--;
            audio.setVolume(vol);
            needsRedraw = true;
        }
        if (tx > RPANEL_X + 62 && tx < RPANEL_X + 108 && vol < MAX_VOLUME) {
            vol++;
            audio.setVolume(vol);
            needsRedraw = true;
        }
    }
}

static bool prevBoot = false;
static bool prevMute = false;
static int  savedVol = DEFAULT_VOLUME;

static void handleButtons() {
    // Boot button — next station
    bool boot = (digitalRead(BTN_BOOT) == LOW);
    if (boot && !prevBoot) {
        currentStation = (currentStation + 1) % NUM_STATIONS;
        songTitle  = "Connecting...";
        songScrollX = 10;
        audio.connecttohost(stationUrls[currentStation]);
        needsRedraw = true;
    }
    prevBoot = boot;

    // Mute button — toggle mute
    bool mute = (digitalRead(BTN_MUTE) == LOW);
    if (mute && !prevMute) {
        if (vol > 0) { savedVol = vol; vol = 0; }
        else         { vol = savedVol; }
        audio.setVolume(vol);
        needsRedraw = true;
    }
    prevMute = mute;
}

// ─── Setup ────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== ESP32-S3-BOX Internet Radio ===");

    Wire.begin(I2C_SDA, I2C_SCL);

    pinMode(BTN_BOOT, INPUT_PULLUP);
    pinMode(BTN_MUTE, INPUT_PULLUP);

    // Enable power amplifier
    pinMode(PA_PIN, OUTPUT);
    digitalWrite(PA_PIN, HIGH);

    initCodec();

    // Display
    tft.init();
    tft.setRotation(0);
    tft.setBrightness(160);
    tft.fillScreen(TFT_BLACK);

    canvas.setColorDepth(16);
    canvas.createSprite(320, 240);
    ticker.setColorDepth(16);
    ticker.createSprite(312, TICKER_H);

    cBg     = canvas.color565(50, 50, 60);
    cPanel  = TFT_BLACK;
    cBorder = canvas.color565(100, 100, 120);
    cAccent = TFT_ORANGE;
    cDim    = canvas.color565(120, 120, 120);
    cBright = canvas.color565(220, 220, 255);

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
    if (wifiConnected)
        Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    else
        Serial.println("WiFi connection failed");

    // Audio
    audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_MCLK);
    audio.setVolume(vol);

    if (wifiConnected) {
        songTitle = "Connecting...";
        audio.connecttohost(stationUrls[currentStation]);
    } else {
        songTitle = "WiFi not connected";
    }

    needsRedraw = true;
}

// ─── Loop ─────────────────────────────────────────────────

void loop() {
    audio.loop();

    unsigned long now = millis();

    // Periodic status update
    if (now - lastStatusMs > 500) {
        lastStatusMs  = now;
        wifiRssi      = WiFi.RSSI();
        wifiConnected = (WiFi.status() == WL_CONNECTED);
        needsRedraw   = true;
    }

    // Scroll song ticker
    if (now - lastTickerMs > 40) {
        lastTickerMs = now;
        drawTicker();
    }

    handleTouch();
    handleButtons();

    if (needsRedraw) drawUI();

    vTaskDelay(1);
}

// ─── Audio callbacks ──────────────────────────────────────

void audio_info(const char *info) {
    Serial.printf("info: %s\n", info);
}

void audio_showstation(const char *info) {
    Serial.printf("station: %s\n", info);
    needsRedraw = true;
}

void audio_showstreamtitle(const char *info) {
    Serial.printf("title: %s\n", info);
    songTitle   = info;
    songScrollX = 310;
    needsRedraw = true;
}

void audio_bitrate(const char *info) {
    Serial.printf("bitrate: %s\n", info);
    bitrate     = String(info).toInt() / 1000;
    needsRedraw = true;
}
