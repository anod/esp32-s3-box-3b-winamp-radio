// ============================================================
// winamp_display — Winamp 2 themed UI for ESP32-S3-BOX-3
// Uses LovyanGFX with PSRAM-backed sprites for flicker-free
// full-frame rendering at ~15fps on Core 1.
// ============================================================

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"

// LGFX_USE_V1 and LGFX_AUTODETECT are set via build flags
#include <LovyanGFX.hpp>

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
  void set_brightness(int brightness) { this->brightness_ = brightness; }

 protected:
  // Rendering
  void draw_frame_();
  void scroll_ticker_();

  // Touch handling (implemented in winamp_touch.cpp)
  void handle_touch_();
  bool read_gt911_home_button_();

  // Winamp button drawing helper
  void draw_wa_btn_(int x, int y, int w, int h, bool pressed,
                    const char *label, int indicator = -1);

  // Component references
  internet_radio::InternetRadio *radio_{nullptr};
  i2s_bridge::I2SBridge *bridge_{nullptr};

  // Display
  LGFX tft_;
  LGFX_Sprite canvas_{&tft_};
  LGFX_Sprite ticker_sprite_{&tft_};

  // Config
  int brightness_{160};
  bool prev_home_btn_{false};

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

  // Timing
  unsigned long last_frame_ms_{0};
  unsigned long last_ticker_ms_{0};
};

}  // namespace winamp_display
}  // namespace esphome
