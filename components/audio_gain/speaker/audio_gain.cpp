#include "audio_gain.h"
#include "gain_logic.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cmath>

namespace esphome::audio_gain {

static const char *const TAG = "audio_gain";

void GainSpeaker::setup() {
  this->output_->add_audio_output_callback([this](uint32_t frames, int64_t timestamp) {
    this->audio_output_callback_.call(frames, timestamp);
  });
}

void GainSpeaker::set_fixed_gain(float gain) {
  this->fixed_ = true;
  this->gain_.store(gain_q8(gain));
}

void GainSpeaker::set_tap_enabled(bool enabled) {
  std::lock_guard<std::mutex> lock(this->tap_mutex_);
  if (enabled && (!this->tap_ || this->tap_fault_.load())) {
    ESP_LOGE(TAG, "Cannot enable missing or failed PCM tap");
    return;
  }
  this->tap_enabled_.store(enabled);
  // Switch restoration can run before the downstream speaker's setup().
  if (!enabled && this->tap_active_) {
    this->tap_->stop();
    this->tap_active_ = false;
  }
}

void GainSpeaker::stop() {
  this->output_->stop();
  std::lock_guard<std::mutex> lock(this->tap_mutex_);
  if (this->tap_active_) {
    this->tap_->stop();
    this->tap_active_ = false;
  }
}

void GainSpeaker::finish() {
  this->output_->finish();
  std::lock_guard<std::mutex> lock(this->tap_mutex_);
  if (this->tap_active_) this->tap_->finish();
}

void GainSpeaker::set_volume(float volume) {
  if (!std::isfinite(volume) || volume < 0.0f || volume > 1.0f) {
    ESP_LOGE(TAG, "Invalid volume: %f", volume);
    return;
  }
  this->requested_volume_.store(volume);
  if (!this->fixed_) this->gain_.store(gain_q8(volume));
}

void GainSpeaker::start() {
  this->output_->set_audio_stream_info(this->audio_stream_info_);
  this->output_->start();
  this->state_ = speaker::STATE_STARTING;
}

size_t GainSpeaker::play(const uint8_t *data, size_t length, TickType_t wait) {
  const auto &info = this->audio_stream_info_;
  const size_t frame_bytes = info.frames_to_bytes(1);
  if (this->output_fault_.load()) return 0;
  if (info.get_bits_per_sample() != 16 || info.get_channels() == 0 ||
      info.get_channels() > 2 || length % frame_bytes != 0) {
    this->output_fault_.store(true);
    ESP_LOGE(TAG, "Expected whole mono/stereo 16-bit PCM frames");
    return 0;
  }
  this->output_->set_audio_stream_info(info);
  const int gain = this->ducked_.load() || (!this->fixed_ && this->muted_.load()) ? 0 : this->gain_.load();
  const size_t written = write_scaled(
      data, length, frame_bytes, gain,
      [this, wait](const uint8_t *scaled, size_t count) { return this->output_->play(scaled, count, wait); },
      [this, &info, frame_bytes](const uint8_t *input, const uint8_t *scaled, size_t count) {
        if (this->pre_gain_tap_) this->pre_gain_tap_(input, count, info);
        {
          std::lock_guard<std::mutex> lock(this->tap_mutex_);
          if (this->tap_enabled_.load() && !this->tap_fault_.load()) {
            this->tap_->set_audio_stream_info(info);
            // The DAC remains the pacing clock; an optional BT branch must never stall it.
            this->tap_active_ = true;
            const size_t tapped = this->tap_->play(scaled, count, 0);
            if (tapped > count || tapped % frame_bytes != 0) {
              this->tap_fault_.store(true);
              ESP_LOGE(TAG, "PCM tap returned an invalid frame count");
            } else {
              this->tap_accepted_bytes_.fetch_add(tapped);
              this->tap_dropped_bytes_.fetch_add(count - tapped);
            }
          }
        }
        uint32_t peak = 0;
        const auto *samples = reinterpret_cast<const int16_t *>(scaled);
        for (size_t i = 0; i < count / 2; ++i) {
          const int value = samples[i];
          peak = std::max(peak, static_cast<uint32_t>(value < 0 ? -value : value));
        }
        this->accepted_bytes_.fetch_add(count);
        this->accepted_peak_.store(std::max(this->accepted_peak_.load(), peak));
      });
  if (written == SIZE_MAX) {
    this->output_fault_.store(true);
    ESP_LOGE(TAG, "Downstream speaker returned an invalid partial frame count");
    return 0;
  }
  return written;
}

void GainSpeaker::loop() {
  if (this->tap_fault_.load() && this->tap_enabled_.load()) {
    this->set_tap_enabled(false);
    this->status_set_warning(LOG_STR("PCM tap failed"));
  }
  if (this->output_fault_.load() && !this->is_failed()) {
    this->stop();
    this->mark_failed();
  }
  if (this->is_failed()) return;
  this->state_ = this->output_->is_stopped() ? speaker::STATE_STOPPED
                : this->output_->is_running() ? speaker::STATE_RUNNING
                                             : speaker::STATE_STARTING;
  if (millis() - this->last_report_ >= 1000) {
    this->last_report_ = millis();
    const auto bytes = this->accepted_bytes_.exchange(0);
    const auto peak = this->accepted_peak_.exchange(0);
    if (bytes != 0)
      ESP_LOGI(TAG, "Path=%s accepted=%u peak=%u", this->fixed_ ? "fixed" : "media",
               static_cast<unsigned>(bytes), static_cast<unsigned>(peak));
    const auto tapped = this->tap_accepted_bytes_.exchange(0);
    const auto dropped = this->tap_dropped_bytes_.exchange(0);
    if (tapped || dropped)
      ESP_LOGI(TAG, "Tap accepted=%u dropped=%u", static_cast<unsigned>(tapped),
               static_cast<unsigned>(dropped));
    if (dropped) ESP_LOGW(TAG, "PCM tap overflow: %u bytes dropped", static_cast<unsigned>(dropped));
  }
}

void GainSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG, "PCM Gain Speaker: %s controls, no shared DAC volume/mute propagation",
                this->fixed_ ? "fixed" : "software");
}

}  // namespace esphome::audio_gain
