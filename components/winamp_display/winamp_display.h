// ============================================================
// winamp_display — Winamp 2 themed UI for ESP32-S3-BOX-3
// Uses LovyanGFX with PSRAM-backed sprites for flicker-free
// full-frame rendering at ~15fps on Core 1.
// ============================================================

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "text_layout.h"
#include "voice_font.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/number/number.h"

#include <LovyanGFX.hpp>

class LGFX final : public lgfx::LGFX_Device {
 public:
  LGFX() {
    auto bus_cfg = this->bus_.config();
    bus_cfg.spi_host = SPI2_HOST;
    bus_cfg.spi_mode = 0;
    bus_cfg.freq_write = 40000000;
    bus_cfg.freq_read = 16000000;
    bus_cfg.spi_3wire = true;
    bus_cfg.use_lock = true;
    bus_cfg.dma_channel = SPI_DMA_CH_AUTO;
    bus_cfg.pin_mosi = 6;
    bus_cfg.pin_miso = -1;
    bus_cfg.pin_sclk = 7;
    bus_cfg.pin_dc = 4;
    this->bus_.config(bus_cfg);

    auto panel_cfg = this->panel_.config();
    panel_cfg.pin_cs = 5;
    panel_cfg.pin_rst = -1;
    panel_cfg.offset_rotation = 1;
    this->panel_.config(panel_cfg);
    this->panel_.setBus(&this->bus_);
    this->panel_.setRotation(1);

    auto light_cfg = this->light_.config();
    light_cfg.pin_bl = 47;
    light_cfg.freq = 12000;
    light_cfg.pwm_channel = 0;
    light_cfg.invert = false;
    this->light_.config(light_cfg);
    this->panel_.setLight(&this->light_);
    this->setPanel(&this->panel_);
  }

 private:
  lgfx::Panel_ILI9342 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
};

// Spectrum analyser — Core 0 captures samples, Core 1 runs FFT
extern float spec_bands[16];
void spectrum_init();
bool spectrum_compute();  // call from draw_frame_ before reading spec_bands

// Forward declarations to avoid including full headers
namespace esphome {
namespace internet_radio {
class InternetRadio;
}
namespace i2s_bridge {
class I2SBridge;
}
}  // namespace esphome

namespace esphome {
namespace winamp_display {

class WinampDisplay final : public Component, public i2c::I2CDevice {
 public:
  float get_setup_priority() const override {
    // Run after InternetRadio (LATE) and I2SBridge (LATE-1)
    return setup_priority::LATE - 2;
  }
  void setup() override;
  void loop() override;

  // Config setters (called by codegen)
  void set_radio(internet_radio::InternetRadio *radio) { this->radio_ = radio; }
  void set_bridge(i2s_bridge::I2SBridge *bridge) { this->bridge_ = bridge; }
  // Voice assistant status (called from YAML on Core 1, same thread as drawing)
  enum VoiceState : uint8_t {
    VOICE_IDLE = 0, VOICE_LOADING, VOICE_LISTENING, VOICE_THINKING, VOICE_REPLYING, VOICE_ERROR
  };
  void set_voice_state(int state) {
    this->voice_hold_ = false;
    if (state == VOICE_IDLE && this->voice_reply_[0] && this->voice_state_ != VOICE_ERROR &&
        this->voice_state_ != VOICE_IDLE) {
      // Keep the reply on screen long enough to read it
      size_t n = strlen(this->voice_reply_);
      unsigned long ms = VOICE_HOLD_MIN_MS + n * VOICE_HOLD_MS_PER_BYTE;
      this->voice_hold_ms_ = ms > VOICE_HOLD_MAX_MS ? VOICE_HOLD_MAX_MS : ms;
      this->voice_hold_ = true;
      this->voice_state_ms_ = millis();
      return;
    }
    this->voice_state_ = (uint8_t) state;
    this->voice_state_ms_ = millis();
    if (state == VOICE_LISTENING || state == VOICE_LOADING) {
      this->voice_heard_[0] = '\0';
      this->voice_reply_[0] = '\0';
      this->voice_layout_dirty_ = true;
    }
    this->last_frame_ms_ = 0;  // switch views on the next loop() pass
  }
  // Debug: freeze the frame and stream the canvas (RGB565) as base64 log lines.
  void dump_screenshot() { this->shot_pos_ = 0; this->shot_active_ = true; }
  bool voice_is_error() const { return this->voice_state_ == VOICE_ERROR; }
  // Recognized speech (shown dim, top) and the assistant's reply / error text.
  void set_voice_text(const char *text) {
    strlcpy(this->voice_heard_, text, sizeof(this->voice_heard_));
    this->voice_layout_dirty_ = true;
  }
  void set_voice_reply(const char *text) {
    strlcpy(this->voice_reply_, text, sizeof(this->voice_reply_));
    this->voice_layout_dirty_ = true;
  }
  void set_brightness_number(number::Number *n) { this->brightness_number_ = n; }
  void set_brightness(int brightness) {
    if (brightness < 1) brightness = 1;
    if (brightness > 255) brightness = 255;
    this->brightness_ = brightness;
    this->screen_on_ = true;
    this->tft_.setBrightness(brightness);
    this->brightness_dirty_ = true;
    this->brightness_dirty_ms_ = millis();
    // Sync preset index to closest level
    for (int i = 0; i < NUM_BRIGHTNESS; i++) {
      if (abs(brightness - BRIGHTNESS_LEVELS[i]) <
          abs(brightness - BRIGHTNESS_LEVELS[this->brightness_idx_]))
        this->brightness_idx_ = i;
    }
    this->publish_brightness_();
  }
  void cycle_brightness();
  void add_on_home_button_callback(std::function<void()> &&cb) {
    this->home_button_cb_.add(std::move(cb));
  }
  int get_brightness() const { return this->brightness_; }

 protected:
  // Rendering
  void draw_frame_();
  void scroll_ticker_();

  // Touch handling (implemented in winamp_touch.cpp)
  void handle_touch_();
  bool read_gt911_home_button_();

  // Publish brightness to HA number entity
  void publish_brightness_() {
    if (this->brightness_number_)
      this->brightness_number_->publish_state((float)this->brightness_);
  }

  // Winamp button drawing helper
  void draw_wa_btn_(int x, int y, int w, int h, bool pressed,
                    const char *label, int indicator = -1);

  // Component references
  internet_radio::InternetRadio *radio_{nullptr};
  i2s_bridge::I2SBridge *bridge_{nullptr};
  number::Number *brightness_number_{nullptr};

  // Display
  LGFX tft_;
  LGFX_Sprite canvas_{&tft_};
  LGFX_Sprite ticker_sprite_{&tft_};
  uint16_t voice_bg_{0};
  uint16_t voice_col_{0};

  // Config
  int brightness_{160};
  int brightness_idx_{2};
  bool screen_on_{true};
  bool prev_home_btn_{false};
  CallbackManager<void()> home_button_cb_;

  uint8_t voice_state_{0};
  unsigned long voice_state_ms_{0};
  char voice_heard_[256]{};
  char voice_reply_[1024]{};  // UTF-8: Cyrillic/Hebrew are 2 bytes per char
  static constexpr int VOICE_HEARD_LINES = 2;
  static constexpr int VOICE_REPLY_LINES = 8;
  static constexpr int VOICE_LINE_BYTES = 128;
  static constexpr int VOICE_TEXT_Y = 96;
  // Pre-wrapped, bidi-reordered lines (rebuilt only when the text changes)
  char voice_vis_[VOICE_HEARD_LINES + VOICE_REPLY_LINES][VOICE_LINE_BYTES]{};
  uint8_t voice_heard_n_{0};
  uint8_t voice_reply_n_{0};
  bool voice_heard_rtl_{false};
  bool voice_reply_rtl_{false};
  bool voice_layout_dirty_{false};
  bool shot_active_{false};
  uint32_t shot_pos_{0};
  void pump_screenshot_();
  // VLW fonts live in flash; only the small glyph tables are heap-allocated, once.
  lgfx::PointerWrapper voice_font_data_;
  lgfx::VLWfont voice_font_;
  lgfx::PointerWrapper voice_font_small_data_;
  lgfx::VLWfont voice_font_small_;
  bool voice_font_ok_{false};
  bool voice_small_ok_{false};
  bool voice_small_{false};  // long text: the small font is in use
  int voice_level_{0};       // smoothed mic meter 0..100
  void draw_voice_view_();
  void layout_voice_text_();
  void layout_voice_lines_(const char *str, int max_lines, uint8_t first_slot, uint8_t *count, bool *rtl,
                           bool *truncated);
  static constexpr unsigned long VOICE_ERROR_SHOW_MS = 4000;
  static constexpr unsigned long VOICE_HOLD_MIN_MS = 4000;
  static constexpr unsigned long VOICE_HOLD_MS_PER_BYTE = 40;
  static constexpr unsigned long VOICE_HOLD_MAX_MS = 15000;
  bool voice_hold_{false};
  unsigned long voice_hold_ms_{0};

  // Brightness presets and auto-dim
  static constexpr uint8_t BRIGHTNESS_LEVELS[] = {20, 80, 160, 255};
  static constexpr int NUM_BRIGHTNESS = 4;
  static constexpr uint8_t DIM_BRIGHTNESS = 10;
  bool dimmed_{false};
  int prev_play_state_{-1};  // track for auto-dim

  // NVS persistence (debounced)
  ESPPreferenceObject brightness_pref_;
  bool brightness_dirty_{false};
  unsigned long brightness_dirty_ms_{0};
  static constexpr unsigned long NVS_SAVE_DEBOUNCE_MS = 3000;

  // Theme colors (computed in setup)
  uint16_t c_bg_, c_panel_, c_border_, c_accent_, c_dim_, c_bright_;
  uint16_t c_station_hl_, c_station_dim_, c_bar_bg_, c_btn_col_;

  // Winamp gradient segment colors (pre-computed)
  static constexpr int MAX_SEGS = 10;
  uint16_t seg_colors_[MAX_SEGS];

  // Layout constants
  static constexpr int LPANEL_W = 198;
  static constexpr int RPANEL_X = 202;
  static constexpr int RPANEL_W = 116;
  static constexpr int HDR_H = 10;
  static constexpr int STA_Y = 14;
  static constexpr int STA_LINE = 18;
  static constexpr int TICKER_Y = 220;
  static constexpr int TICKER_H = 18;
  static constexpr int TICKER_REGION_Y = 206;
  static constexpr int TICKER_BOX_Y = 218;
  static constexpr int CANVAS_H = 218;
  static constexpr int VIZ_W = RPANEL_W;
  static constexpr int VIZ_H = 38;
  static constexpr int VIZ_X = RPANEL_X;
  static constexpr int VIZ_Y = HDR_H + 4 + 60;

  // Winamp button palette
  static constexpr uint16_t WA_BTN_FACE = 0xC618;
  static constexpr uint16_t WA_HIGHLIGHT = 0xFFFF;
  static constexpr uint16_t WA_SHADOW = 0x8410;
  static constexpr uint16_t WA_BORDER = 0x2104;
  static constexpr uint16_t WA_TEST_DOT = 0xFD00;  // orange (0xFF, 0xA0, 0x00)

  // Ticker state
  static constexpr int TICKER_SPRITE_W = 316;
  static constexpr int TICKER_SPRITE_H = 18;
  int ticker_cursor_x_{TICKER_SPRITE_W};
  char ticker_prev_title_[128]{};

  // Visualizer state
  static constexpr int VIZ_BANDS = 16;
  static constexpr int SEG_H = 2;
  static constexpr int SEG_GAP = 1;
  float display_bars_[VIZ_BANDS]{};

  // Touch state
  int touch_highlight_{-1};
  unsigned long touch_hl_ms_{0};
  unsigned long last_touch_ms_{0};
  int saved_vol_{15};
  bool touch_detected_{false};
  int touch_x_{0};
  int touch_y_{0};

  // Station list toggle — requires 5 taps within 3s
  int list_tap_count_{0};
  unsigned long list_tap_ms_{0};
  static constexpr int LIST_TAP_REQUIRED = 5;
  static constexpr unsigned long LIST_TAP_WINDOW_MS = 3000;

  // Timing
  unsigned long last_frame_ms_{0};
  unsigned long last_ticker_ms_{0};
  unsigned long last_touch_ms_loop_{0};
};

class HomeButtonTrigger final : public Trigger<> {
 public:
  explicit HomeButtonTrigger(WinampDisplay *parent) {
    parent->add_on_home_button_callback([this]() { this->trigger(); });
  }
};

}  // namespace winamp_display
}  // namespace esphome
