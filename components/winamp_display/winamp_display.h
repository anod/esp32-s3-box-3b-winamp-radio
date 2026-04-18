// ============================================================
// winamp_display — Winamp 2 themed UI for ESP32-S3-BOX-3
// Uses LovyanGFX with PSRAM-backed sprites for flicker-free
// full-frame rendering at ~15fps on Core 1.
// ============================================================

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/number/number.h"

// LGFX_USE_V1 and LGFX_AUTODETECT are set via build flags
#include <LovyanGFX.hpp>

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

  // Config
  int brightness_{160};
  int brightness_idx_{2};
  bool screen_on_{true};
  bool prev_home_btn_{false};

  // Brightness presets and auto-dim
  static constexpr uint8_t BRIGHTNESS_LEVELS[] = {20, 80, 160, 255};
  static constexpr int NUM_BRIGHTNESS = 4;
  static constexpr uint8_t DIM_BRIGHTNESS = 10;
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

}  // namespace winamp_display
}  // namespace esphome
