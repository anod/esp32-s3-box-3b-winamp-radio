#include "winamp_display.h"
#include "esphome/components/internet_radio/media_player/internet_radio.h"
#include "esphome/components/i2s_bridge/switch/i2s_bridge.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/components/network/util.h"
#include "esphome/components/network/ip_address.h"
#include <math.h>

namespace esphome {
namespace winamp_display {

static const char *const TAG = "winamp_display";

// Winamp 2 gradient: 6 key colors from base (dark blue-green) to peak (red)
static const uint8_t grad_r[] = {0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
static const uint8_t grad_g[] = {0x40, 0x80, 0xFF, 0xFF, 0x80, 0x00};
static const uint8_t grad_b[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x00};

static uint16_t gradient_color_565(float t) {
  if (t <= 0.0f) t = 0.0f;
  if (t >= 1.0f) t = 1.0f;
  float pos = t * 5.0f;
  int idx = (int)pos;
  if (idx > 4) idx = 4;
  float f = pos - idx;
  uint8_t r = grad_r[idx] + (grad_r[idx + 1] - grad_r[idx]) * f;
  uint8_t g = grad_g[idx] + (grad_g[idx + 1] - grad_g[idx]) * f;
  uint8_t b = grad_b[idx] + (grad_b[idx + 1] - grad_b[idx]) * f;
  return lgfx::color565(r, g, b);
}

void WinampDisplay::setup() {
  ESP_LOGI(TAG, "Initializing display...");

  // Probe GT911 address (0x14 or 0x5D depending on INT pin state at reset)
  // Uses ESPHome's I2C bus — the default address (0x14) is set by codegen.
  // Try reading status register; if it fails, try alternate address.
  {
    uint8_t dummy = 0;
    if (this->read_register16(0x814E, &dummy, 1) != i2c::ERROR_OK) {
      this->set_i2c_address(0x5D);
      if (this->read_register16(0x814E, &dummy, 1) != i2c::ERROR_OK) {
        this->set_i2c_address(0x14);  // fallback
      }
    }
    ESP_LOGI(TAG, "GT911 address: 0x%02X", this->address_);
  }

  this->tft_.init();
  this->tft_.setBrightness(this->brightness_);

  // Restore saved brightness from NVS
  this->brightness_pref_ = global_preferences->make_preference<int>(fnv1_hash("disp_bri"));
  int saved_bri = 0;
  if (this->brightness_pref_.load(&saved_bri) && saved_bri >= 1 && saved_bri <= 255) {
    this->brightness_ = saved_bri;
    // Sync brightness_idx_ to closest preset
    for (int i = 0; i < NUM_BRIGHTNESS; i++) {
      if (abs(saved_bri - BRIGHTNESS_LEVELS[i]) <
          abs(saved_bri - BRIGHTNESS_LEVELS[this->brightness_idx_]))
        this->brightness_idx_ = i;
    }
    this->tft_.setBrightness(this->brightness_);
    ESP_LOGI(TAG, "Restored brightness: %d", this->brightness_);
  }

  // Create PSRAM-backed sprites
  this->canvas_.setColorDepth(16);
  this->canvas_.setPsram(true);  // 320×218×2 = 140KB needs PSRAM
  void *buf = this->canvas_.createSprite(320, CANVAS_H);
  ESP_LOGI(TAG, "Canvas sprite: %p (%dx%d = %d bytes)", buf, 320, CANVAS_H, 320 * CANVAS_H * 2);

  this->ticker_sprite_.setColorDepth(16);
  this->ticker_sprite_.createSprite(TICKER_SPRITE_W, TICKER_SPRITE_H);

  // Pre-compute theme colors — match original Winamp 2 palette
  this->c_bg_ = lgfx::color565(50, 50, 60);
  this->c_panel_ = TFT_BLACK;
  this->c_border_ = lgfx::color565(100, 100, 120);
  this->c_accent_ = TFT_ORANGE;
  this->c_dim_ = lgfx::color565(120, 120, 120);
  this->c_bright_ = lgfx::color565(220, 220, 255);
  this->c_station_hl_ = lgfx::color565(0, 40, 0);
  this->c_station_dim_ = lgfx::color565(0, 140, 0);
  this->c_bar_bg_ = lgfx::color565(60, 60, 60);
  this->c_btn_col_ = lgfx::color565(80, 80, 100);

  // Pre-compute visualizer gradient
  for (int i = 0; i < MAX_SEGS; i++) {
    this->seg_colors_[i] = gradient_color_565((float)i / (MAX_SEGS - 1));
  }

  // Initialize FFT tables (must happen before audio starts streaming)
  spectrum_init();

  // Initial draw
  this->draw_frame_();

  ESP_LOGI(TAG, "Display initialized");
}

// constexpr static member definition
constexpr uint8_t WinampDisplay::BRIGHTNESS_LEVELS[];

void WinampDisplay::cycle_brightness() {
  this->brightness_idx_ = (this->brightness_idx_ + 1) % NUM_BRIGHTNESS;
  this->set_brightness(BRIGHTNESS_LEVELS[this->brightness_idx_]);
}

void WinampDisplay::loop() {
  unsigned long now = millis();

  // Clear touch highlight after 200ms
  if (this->touch_highlight_ >= 0 && (now - this->touch_hl_ms_ >= 200)) {
    this->touch_highlight_ = -1;
  }

  // Touch input — rate-limited to 20ms (50Hz) to reduce I2C bus contention
  if (now - this->last_touch_ms_loop_ >= 20) {
    this->last_touch_ms_loop_ = now;
    this->handle_touch_();
  }

  // Main frame + visualizer (~15fps = 66ms) — single canvas push
  if (now - this->last_frame_ms_ >= 66) {
    this->last_frame_ms_ = now;
    this->draw_frame_();
  }

  // Ticker scroll (~20Hz)
  if (now - this->last_ticker_ms_ >= 50) {
    this->last_ticker_ms_ = now;
    this->scroll_ticker_();
  }

  // Auto-dim: dim when stopped, restore when playing
  if (this->radio_) {
    int ps = static_cast<int>(this->radio_->get_play_state());
    if (ps != this->prev_play_state_) {
      if (this->screen_on_) {
        if (ps == static_cast<int>(internet_radio::PS_STOPPED))
          this->tft_.setBrightness(DIM_BRIGHTNESS);
        else if (this->prev_play_state_ == static_cast<int>(internet_radio::PS_STOPPED))
          this->tft_.setBrightness(this->brightness_);
      }
      this->prev_play_state_ = ps;
    }
  }

  // Debounced NVS brightness save
  if (this->brightness_dirty_ && (now - this->brightness_dirty_ms_ >= NVS_SAVE_DEBOUNCE_MS)) {
    this->brightness_dirty_ = false;
    this->brightness_pref_.save(&this->brightness_);
    ESP_LOGD(TAG, "NVS: saved brightness=%d", this->brightness_);
  }
}

void WinampDisplay::draw_wa_btn_(int bx, int by, int bw, int bh, bool pressed,
                                  const char *label, int indicator) {
  auto &c = this->canvas_;
  c.fillRect(bx, by, bw, bh, WA_BTN_FACE);
  c.drawRect(bx, by, bw, bh, WA_BORDER);
  if (!pressed) {
    c.drawFastHLine(bx + 1, by + 1, bw - 3, WA_HIGHLIGHT);
    c.drawFastVLine(bx + 1, by + 1, bh - 3, WA_HIGHLIGHT);
    c.drawFastHLine(bx + 1, by + bh - 2, bw - 2, WA_SHADOW);
    c.drawFastVLine(bx + bw - 2, by + 1, bh - 2, WA_SHADOW);
  } else {
    c.drawRect(bx + 1, by + 1, bw - 2, bh - 2, WA_SHADOW);
  }
  int off = pressed ? 1 : 0;
  c.setTextColor(WA_BORDER, WA_BTN_FACE);
  if (indicator >= 0) {
    int ix = bx + 3 + off, iy = by + (bh - 5) / 2 + off;
    c.fillRect(ix, iy, 5, 5, indicator ? TFT_GREEN : WA_SHADOW);
    c.drawRect(ix, iy, 5, 5, WA_BORDER);
    c.drawString(label, ix + 6, by + (bh / 2) - 3 + off, 1);
  } else {
    c.drawCenterString(label, bx + bw / 2 + off, by + (bh / 2) - 3 + off, 1);
  }
}

void WinampDisplay::draw_frame_() {
  auto &c = this->canvas_;
  if (!c.getBuffer()) return;
  c.fillSprite(this->c_bg_);

  // Read state from InternetRadio
  int current_station = this->radio_ ? this->radio_->get_current_station() : 0;
  int num_stations = internet_radio::InternetRadio::get_num_stations();
  int vol = this->radio_ ? this->radio_->get_vol() : 0;
  int max_vol = internet_radio::InternetRadio::get_max_volume();
  bool muted = this->radio_ ? this->radio_->is_muted() : false;
  long bitrate = this->radio_ ? this->radio_->get_bitrate() : 0;
  bool bt_mode = i2s_bridge::I2SBridge::is_active();

  // WiFi state from ESPHome
  bool wifi_connected = network::is_connected();
  int8_t wifi_rssi = 0;
  if (wifi_connected && wifi::global_wifi_component != nullptr) {
    wifi_rssi = wifi::global_wifi_component->wifi_rssi();
  }

  // ── Left panel: station list ──
  c.fillRect(2, HDR_H + 4, LPANEL_W - 4, num_stations * STA_LINE + 8, this->c_panel_);
  c.drawRect(2, HDR_H + 4, LPANEL_W - 4, num_stations * STA_LINE + 8, this->c_border_);

  // ── Winamp 2 title bar ──
  {
    const int tm = 4, lm = 6;
    const int ly1 = tm + 1, ly2 = ly1 + 4, lh = 2;

    c.setTextColor(this->c_accent_, this->c_bg_);
    c.drawString("R", lm, tm, 1);

    int bx = 320 - lm - 8;
    c.drawLine(bx + 2, tm + 2, bx + 5, tm + 5, this->c_accent_);
    c.drawLine(bx + 5, tm + 2, bx + 2, tm + 5, this->c_accent_);
    bx -= 9;
    c.drawFastHLine(bx + 2, tm + 4, 4, this->c_accent_);
    bx -= 9;
    c.drawRect(bx + 2, tm + 2, 4, 4, this->c_accent_);

    int lines_l = lm + 8 + 4;
    int lines_r = bx - 2;
    const char *title = "ESP32 RADIO";
    int title_w = 67;
    int title_x = 160;
    int left_end = title_x - title_w / 2 - 4;
    int right_start = title_x + title_w / 2 + 4;

    c.fillRect(lines_l, ly1, left_end - lines_l, lh, this->c_accent_);
    c.fillRect(lines_l, ly2, left_end - lines_l, lh, this->c_accent_);
    c.fillRect(right_start, ly1, lines_r - right_start, lh, this->c_accent_);
    c.fillRect(right_start, ly2, lines_r - right_start, lh, this->c_accent_);

    c.setTextColor(this->c_bright_, this->c_bg_);
    c.drawCenterString(title, title_x, tm, 1);
    c.drawCenterString(title, title_x + 1, tm, 1);
  }

  // Station list
  for (int i = 0; i < num_stations; i++) {
    int y = STA_Y + 6 + i * STA_LINE;
    if (i == current_station) {
      c.fillRect(4, y - 1, LPANEL_W - 8, STA_LINE - 2, this->c_station_hl_);
      c.setTextColor(TFT_GREEN, this->c_station_hl_);
      c.drawString(">", 8, y, 2);
    } else {
      c.setTextColor(this->c_station_dim_, this->c_panel_);
    }
    c.drawString(internet_radio::InternetRadio::get_station_name_at(i), 24, y, 2);
  }

  // Divider
  c.fillRect(LPANEL_W, HDR_H + 4, 3, num_stations * STA_LINE + 8, this->c_border_);

  // ── Right panel: WiFi ──
  int ry = HDR_H + 4;
  c.fillRect(RPANEL_X, ry, RPANEL_W, 56, this->c_panel_);
  c.drawRect(RPANEL_X, ry, RPANEL_W, 56, this->c_border_);

  c.setTextColor(this->c_dim_, this->c_panel_);
  c.drawString("WIFI", RPANEL_X + 6, ry + 4, 1);

  // Station list indicator — barely visible dot, top-right of WiFi panel
  // Dim when normal list, orange when test list active
  {
    bool test_mode = this->radio_ && this->radio_->is_test_list();
    c.fillRect(RPANEL_X + RPANEL_W - 8, ry + 3, 3, 3, test_mode ? WA_TEST_DOT : this->c_border_);
  }

  {
    int bars = 0;
    if (wifi_connected) {
      if (wifi_rssi >= -50) bars = 4;
      else if (wifi_rssi >= -60) bars = 3;
      else if (wifi_rssi >= -70) bars = 2;
      else bars = 1;
    }
    int bx = RPANEL_X + 32, base_y = ry + 12;
    for (int i = 0; i < 4; i++) {
      int bh = 2 + i * 2;
      c.fillRect(bx + i * 3, base_y - bh, 2, bh, (i < bars) ? TFT_GREEN : this->c_dim_);
    }
  }

  c.setTextColor(wifi_connected ? TFT_GREEN : TFT_RED, this->c_panel_);
  c.drawString(wifi_connected ? "CONNECTED" : "OFFLINE", RPANEL_X + 6, ry + 16, 1);
  c.setTextColor(this->c_bright_, this->c_panel_);
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "RSSI:%d", wifi_rssi);
    c.drawString(buf, RPANEL_X + 6, ry + 28, 1);
  }
  if (wifi_connected) {
    // Cache IP string — use ESPHome network API
    static char ip_str[16] = "";
    static unsigned long last_ip_ms = 0;
    unsigned long now = millis();
    if (now - last_ip_ms > 10000 || ip_str[0] == '\0') {
      last_ip_ms = now;
      auto addrs = network::get_ip_addresses();
      if (!addrs.empty()) {
        addrs[0].str_to(ip_str);
      }
    }
    c.setTextColor(this->c_dim_, this->c_panel_);
    c.drawString(ip_str, RPANEL_X + 6, ry + 42, 1);
  }

  // ── Right panel: Volume ──
  int gy = ry + 60;
  int vy = gy + 42;
  c.fillRect(RPANEL_X, vy, RPANEL_W, 52, this->c_panel_);
  c.drawRect(RPANEL_X, vy, RPANEL_W, 52, this->c_border_);

  c.setTextColor(this->c_dim_, this->c_panel_);
  {
    char vol_label[20];
    snprintf(vol_label, sizeof(vol_label), "VOLUME: %d%%", (vol * 100 + max_vol / 2) / max_vol);
    c.drawCenterString(vol_label, RPANEL_X + RPANEL_W / 2, vy + 3, 1);
  }

  int bar_w = RPANEL_W - 16;
  int bar_x = RPANEL_X + 8;
  int bar_y = vy + 18;
  c.fillRoundRect(bar_x, bar_y, bar_w, 4, 2, this->c_bar_bg_);
  int fill_w = (vol * bar_w) / max_vol;
  c.fillRoundRect(bar_x, bar_y, fill_w, 4, 2, gradient_color_565((float)vol / max_vol));
  int knob_x = bar_x + fill_w - 5;
  if (knob_x < bar_x) knob_x = bar_x;
  {
    int kw = 10, kh = 12, ky = bar_y - 4;
    c.fillRect(knob_x, ky, kw, kh, WA_BTN_FACE);
    c.drawRect(knob_x, ky, kw, kh, WA_BORDER);
    c.drawFastHLine(knob_x + 1, ky + 1, kw - 3, WA_HIGHLIGHT);
    c.drawFastVLine(knob_x + 1, ky + 1, kh - 3, WA_HIGHLIGHT);
    c.drawFastHLine(knob_x + 1, ky + kh - 2, kw - 2, WA_SHADOW);
    c.drawFastVLine(knob_x + kw - 2, ky + 1, kh - 2, WA_SHADOW);
  }

  // Volume buttons
  bool hl_active = (this->touch_highlight_ >= 0 && (millis() - this->touch_hl_ms_ < 200));
  {
    int btn_y = vy + 34, btn_h = 15;
    this->draw_wa_btn_(RPANEL_X + 4, btn_y, 34, btn_h,
                       (hl_active && this->touch_highlight_ == 0), "-");
    this->draw_wa_btn_(RPANEL_X + 40, btn_y, 36, btn_h,
                       (hl_active && this->touch_highlight_ == 1), "MUTE", muted ? 1 : 0);
    this->draw_wa_btn_(RPANEL_X + 78, btn_y, 34, btn_h,
                       (hl_active && this->touch_highlight_ == 2), "+");
  }

  // ── Right panel: Bitrate ──
  int by = vy + 56;
  c.fillRect(RPANEL_X, by, RPANEL_W, 16, this->c_panel_);
  c.drawRect(RPANEL_X, by, RPANEL_W, 16, this->c_border_);
  c.setTextColor(TFT_GREEN, this->c_panel_);
  {
    char buf[24];
    snprintf(buf, sizeof(buf), "BITRATE %ldk", bitrate);
    c.drawString(buf, RPANEL_X + 6, by + 4, 1);
  }

  // ── BT Speaker toggle ──
  int bty = by + 18;
  this->draw_wa_btn_(RPANEL_X + 4, bty, RPANEL_W - 8, 16,
                     (hl_active && this->touch_highlight_ == 4),
                     "BT SPEAKER", bt_mode ? 1 : 0);

  // ── "NOW PLAYING" label ──
  c.setTextColor(this->c_dim_, this->c_bg_);
  c.drawString("NOW PLAYING", 6, TICKER_REGION_Y, 1);

  // ── Visualizer panel (drawn into canvas — no separate sprite push) ──
  c.fillRect(VIZ_X, VIZ_Y, VIZ_W, VIZ_H, this->c_panel_);
  c.drawRect(VIZ_X, VIZ_Y, VIZ_W, VIZ_H, this->c_border_);

  if (wifi_connected) {
    // Run FFT on Core 1 (zero overhead on audio core)
    spectrum_compute();

    // Smooth spectrum bars: fast attack (0.6), slow decay (0.15)
    // spec_bands contains magnitude² — log10 scaling adjusted accordingly
    for (int i = 0; i < VIZ_BANDS; i++) {
      float target = spec_bands[i];
      // magnitude² threshold: 10000 (= 100²), scale by 0.65 (half of 1.3 for sqrt)
      target = (target > 10000.0f) ? (log10f(target) - 4.0f) * 0.65f : 0.0f;
      if (target > 5.0f) target = 5.0f;
      if (target < 0.0f) target = 0.0f;
      if (target > this->display_bars_[i])
        this->display_bars_[i] += (target - this->display_bars_[i]) * 0.6f;
      else
        this->display_bars_[i] += (target - this->display_bars_[i]) * 0.15f;
    }

    int bar_w = 5, gap = 2;
    int total_w = VIZ_BANDS * bar_w + (VIZ_BANDS - 1) * gap;
    int start_x = VIZ_X + (VIZ_W - total_w) / 2;
    int seg_step = SEG_H + SEG_GAP;
    int base_y = VIZ_Y + VIZ_H - 3;
    for (int i = 0; i < VIZ_BANDS; i++) {
      int num_segs = (int)(this->display_bars_[i] * (MAX_SEGS / 5.0f) + 0.5f);
      if (num_segs > MAX_SEGS) num_segs = MAX_SEGS;
      int bx = start_x + i * (bar_w + gap);
      for (int s = 0; s < num_segs; s++) {
        int sy = base_y - (s + 1) * seg_step + SEG_GAP;
        c.fillRect(bx, sy, bar_w, SEG_H, this->seg_colors_[s]);
      }
    }
  }

  // Outer frame
  c.drawRect(0, 0, 320, CANVAS_H, this->c_border_);

  c.pushSprite(0, 0);
}

void WinampDisplay::scroll_ticker_() {
  const char *title = this->radio_ ? this->radio_->get_song_title() : "";

  // Detect title change — restart from right edge
  if (strcmp(this->ticker_prev_title_, title) != 0) {
    strlcpy(this->ticker_prev_title_, title, sizeof(this->ticker_prev_title_));
    this->ticker_cursor_x_ = TICKER_SPRITE_W;
  }

  auto &ts = this->ticker_sprite_;
  ts.fillSprite(this->c_panel_);
  ts.drawRect(0, 0, TICKER_SPRITE_W, TICKER_SPRITE_H, this->c_border_);
  ts.setTextColor(TFT_GREEN, this->c_panel_);
  ts.drawString(title, this->ticker_cursor_x_, 3, 2);
  this->ticker_cursor_x_ -= 1;

  int text_w = ts.textWidth(title, &fonts::Font2);
  if (this->ticker_cursor_x_ < -text_w) {
    this->ticker_cursor_x_ = TICKER_SPRITE_W;
  }

  ts.pushSprite(2, TICKER_BOX_Y);
}

}  // namespace winamp_display
}  // namespace esphome
