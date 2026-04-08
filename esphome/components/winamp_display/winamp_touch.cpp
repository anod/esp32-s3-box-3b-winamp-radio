#include "winamp_display.h"
#include "esphome/components/internet_radio/media_player/internet_radio.h"
#include "esphome/components/i2s_bridge/switch/i2s_bridge.h"

namespace esphome {
namespace winamp_display {

static const char *const TAG_TOUCH = "winamp_touch";

bool WinampDisplay::read_gt911_home_button_() {
  // Read GT911 status register 0x814E via lgfx::i2c
  // (LovyanGFX's tft_.getTouch() doesn't work because ESPHome owns
  // the I2C bus and LovyanGFX failed to initialize its own bus driver)
  uint8_t reg[2] = {0x81, 0x4E};
  uint8_t status = 0;

  if (!lgfx::i2c::transactionWriteRead(0, this->gt911_addr_, reg, 2, &status, 1).has_value())
    return false;

  if (!(status & 0x80)) return false;  // No data ready

  bool home_key = (status & 0x10) != 0;
  int num_touches = status & 0x0F;

  // Read coordinate touch if available (register 0x8150-0x8153)
  if (num_touches > 0 && num_touches <= 5) {
    uint8_t treg[2] = {0x81, 0x50};
    uint8_t tdata[4] = {};
    if (lgfx::i2c::transactionWriteRead(0, this->gt911_addr_, treg, 2, tdata, 4).has_value()) {
      this->touch_x_ = tdata[0] | (tdata[1] << 8);
      this->touch_y_ = tdata[2] | (tdata[3] << 8);
      this->touch_detected_ = true;
    }
  }

  // Clear status register so GT911 can report new events
  uint8_t clear[3] = {0x81, 0x4E, 0x00};
  lgfx::i2c::transactionWrite(0, this->gt911_addr_, clear, 3);

  return home_key;
}

void WinampDisplay::handle_touch_() {
  using namespace internet_radio;

  this->touch_detected_ = false;

  // Read GT911 — gets home button state AND coordinate touch in one pass
  bool home_btn = this->read_gt911_home_button_();

  // Home button (red circle below screen)
  if (home_btn && !this->prev_home_btn_) {
    unsigned long now = millis();
    if (now - this->last_touch_ms_ >= 300) {
      this->last_touch_ms_ = now;
      ESP_LOGD(TAG_TOUCH, "Home button pressed");
      if (this->radio_) {
        PlayState ps = this->radio_->get_play_state();
        if (ps == PS_PLAYING || ps == PS_PAUSED) {
          auto call = this->radio_->make_call();
          call.set_command(media_player::MEDIA_PLAYER_COMMAND_STOP);
          call.perform();
        } else {
          auto call = this->radio_->make_call();
          call.set_command(media_player::MEDIA_PLAYER_COMMAND_PLAY);
          call.perform();
        }
      }
    }
  }
  this->prev_home_btn_ = home_btn;

  // Coordinate touch
  if (!this->touch_detected_) return;

  unsigned long now = millis();
  if (now - this->last_touch_ms_ < 300) return;
  this->last_touch_ms_ = now;

  int tx = this->touch_x_, ty = this->touch_y_ + 12;  // GT911 Y offset

  int num_stations = InternetRadio::get_num_stations();
  int max_vol = InternetRadio::get_max_volume();

  // Station selection — left panel
  if (tx < LPANEL_W && ty > STA_Y && ty < STA_Y + num_stations * STA_LINE + 10) {
    int idx = (ty - STA_Y - 4) / STA_LINE;
    if (this->radio_ && idx >= 0 && idx < num_stations &&
        idx != this->radio_->get_current_station()) {
      this->radio_->set_station(idx);
      ESP_LOGD(TAG_TOUCH, "Station: %s", InternetRadio::get_station_name_at(idx));
    }
    return;
  }

  // Volume slider — right panel
  int ry = HDR_H + 4;
  int gy = ry + 60;
  int vy = gy + 42;
  int bar_x = RPANEL_X + 4;
  int bar_w = RPANEL_W - 8;
  if (tx >= RPANEL_X && tx <= RPANEL_X + RPANEL_W && ty >= vy + 10 && ty <= vy + 26) {
    int clamped_x = tx;
    if (clamped_x < bar_x) clamped_x = bar_x;
    if (clamped_x > bar_x + bar_w) clamped_x = bar_x + bar_w;
    int new_vol = ((clamped_x - bar_x) * max_vol + bar_w / 2) / bar_w;
    if (new_vol < 0) new_vol = 0;
    if (new_vol > max_vol) new_vol = max_vol;
    if (this->radio_) {
      this->radio_->set_volume_direct(new_vol);
    }
    this->touch_highlight_ = 3;
    this->touch_hl_ms_ = millis();
    return;
  }

  // Volume buttons: [ - ] [ MUTE ] [ + ] at vy+34
  if (tx > RPANEL_X && ty >= vy + 30 && ty <= vy + 54) {
    int vol = this->radio_ ? this->radio_->get_vol() : 0;
    if (tx < RPANEL_X + 39 && vol > 0) {
      // Volume down
      if (this->radio_) this->radio_->set_volume_direct(vol - 1);
      this->touch_highlight_ = 0;
      this->touch_hl_ms_ = millis();
    } else if (tx >= RPANEL_X + 39 && tx < RPANEL_X + 77) {
      // Mute toggle — use is_muted() not vol to handle HA-initiated mutes
      if (this->radio_ && !this->radio_->is_muted()) {
        this->saved_vol_ = this->radio_->get_vol();
        this->radio_->set_volume_direct(0);
      } else if (this->radio_) {
        this->radio_->set_volume_direct(this->saved_vol_);
      }
      this->touch_highlight_ = 1;
      this->touch_hl_ms_ = millis();
    } else if (tx >= RPANEL_X + 77 && vol < max_vol) {
      // Volume up
      if (this->radio_) this->radio_->set_volume_direct(vol + 1);
      this->touch_highlight_ = 2;
      this->touch_hl_ms_ = millis();
    }
    return;
  }

  // BT Speaker toggle
  int by = vy + 56;
  int bty = by + 18;
  if (tx >= RPANEL_X && tx <= RPANEL_X + RPANEL_W && ty >= bty && ty <= bty + 16) {
    if (this->bridge_) {
      bool active = i2s_bridge::I2SBridge::is_active();
      if (active) {
        this->bridge_->turn_off();
      } else {
        this->bridge_->turn_on();
      }
    }
    this->touch_highlight_ = 4;
    this->touch_hl_ms_ = millis();
    ESP_LOGD(TAG_TOUCH, "BT Speaker toggled");
    return;
  }
}

}  // namespace winamp_display
}  // namespace esphome
