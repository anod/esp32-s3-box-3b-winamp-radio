// ============================================================
// I2S Bridge — component implementation
// ============================================================

#include "i2s_bridge.h"
#include "esphome/core/log.h"

namespace esphome {
namespace i2s_bridge {

static const char *const TAG = "i2s_bridge";

// Static members
i2s_chan_handle_t I2SBridge::tx_handle_ = nullptr;
volatile bool I2SBridge::active_ = false;
volatile uint32_t I2SBridge::bridge_sample_rate = 44100;

i2s_chan_handle_t I2SBridge::get_tx_handle() { return tx_handle_; }
bool I2SBridge::is_active() { return active_; }

void I2SBridge::setup() {
  // Restore previous state
  bool initial_state = false;
  // Start disabled by default — user toggles via HA
  this->publish_state(initial_state);
  if (initial_state) {
    this->init_i2s_();
  }
}

void I2SBridge::write_state(bool state) {
  if (state && !tx_handle_) {
    this->init_i2s_();
    active_ = true;
    ESP_LOGI(TAG, "I2S bridge enabled");
  } else if (!state && tx_handle_) {
    active_ = false;
    this->deinit_i2s_();
    ESP_LOGI(TAG, "I2S bridge disabled");
  }
  this->publish_state(state);
}

void I2SBridge::init_i2s_() {
  if (tx_handle_) return;

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 16;
  chan_cfg.dma_frame_num = 480;
  chan_cfg.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)this->bclk_pin_,
          .ws = (gpio_num_t)this->lrck_pin_,
          .dout = (gpio_num_t)this->dout_pin_,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
      },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));

  ESP_LOGI(TAG, "I2S1: BCLK=%d LRCK=%d DOUT=%d DMA=%d frames",
           this->bclk_pin_, this->lrck_pin_, this->dout_pin_, 16 * 480);
}

void I2SBridge::deinit_i2s_() {
  i2s_chan_handle_t h = tx_handle_;
  tx_handle_ = nullptr;
  if (h) {
    i2s_channel_disable(h);
    i2s_del_channel(h);
  }
}

}  // namespace i2s_bridge
}  // namespace esphome
