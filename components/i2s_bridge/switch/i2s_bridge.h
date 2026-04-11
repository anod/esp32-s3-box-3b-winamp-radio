// ============================================================
// I2S Bridge — ESPHome switch component
// Manages I2S1 TX to WROOM-32D A2DP bridge board.
// audio_process_i2s weak override is in a separate file.
// ============================================================

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "driver/i2s_std.h"

namespace esphome {
namespace i2s_bridge {

class I2SBridge final : public switch_::Switch, public Component {
 public:
  float get_setup_priority() const override { return setup_priority::LATE - 1; }
  void setup() override;

  void set_bclk_pin(int pin) { this->bclk_pin_ = pin; }
  void set_lrck_pin(int pin) { this->lrck_pin_ = pin; }
  void set_dout_pin(int pin) { this->dout_pin_ = pin; }
  void set_pa_pin(int pin) { this->pa_pin_ = pin; }

  // Called from audio_process_i2s (separate TU)
  static i2s_chan_handle_t get_tx_handle();
  static bool is_active();
  static volatile uint32_t bridge_sample_rate;

 protected:
  void write_state(bool state) override;
  void init_i2s_();
  void deinit_i2s_();

  int bclk_pin_{10};
  int lrck_pin_{14};
  int dout_pin_{11};
  int pa_pin_{-1};

  static i2s_chan_handle_t tx_handle_;
  static volatile bool active_;
};

}  // namespace i2s_bridge
}  // namespace esphome
