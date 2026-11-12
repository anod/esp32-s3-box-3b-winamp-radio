#include "audio_gain.h"
#include "gain_logic.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <cmath>

namespace esphome::audio_gain {

static const char *const TAG = "audio_gain";

void GainSpeaker::setup() {
  if (this->tap_) {
    auto *storage = static_cast<uint8_t *>(heap_caps_malloc(TAP_QUEUE_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!storage) {
      ESP_LOGE(TAG, "Failed to allocate PCM tap queue");
      this->tap_fault_.store(true);
    } else {
      this->tap_queue_.set_storage(storage, TAP_QUEUE_CAPACITY);
    }
  }
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
  if (!enabled) this->reset_tap_();
}

void GainSpeaker::reset_tap_() {
  this->tap_epoch_.fetch_add(1);
  if (this->tap_queue_.size())
    ESP_LOGI(TAG, "PCM tap discarded %u queued bytes", static_cast<unsigned>(this->tap_queue_.size()));
  this->tap_queue_.clear();
  this->tap_finishing_ = false;
  // Switch restoration can run before the downstream speaker's setup().
  if (this->tap_active_) {
    this->tap_->stop();
    this->tap_active_ = false;
  }
}

void GainSpeaker::set_tap_suspended(bool suspended) {
  std::lock_guard<std::mutex> lock(this->tap_mutex_);
  this->tap_suspended_ = suspended;
  if (suspended || this->tap_finishing_) this->reset_tap_();
}

void GainSpeaker::set_mute_state(bool muted) {
  std::lock_guard<std::mutex> lock(this->tap_mutex_);
  if (muted && !this->muted_.load()) this->reset_tap_();
  this->muted_.store(muted);
}

void GainSpeaker::set_ducked(bool ducked) {
  std::lock_guard<std::mutex> lock(this->tap_mutex_);
  if (ducked && !this->ducked_.load()) this->reset_tap_();
  this->ducked_.store(ducked);
}

void GainSpeaker::stop() {
  this->output_->stop();
  std::lock_guard<std::mutex> lock(this->tap_mutex_);
  this->reset_tap_();
}

void GainSpeaker::finish() {
  this->output_->finish();
  std::lock_guard<std::mutex> lock(this->tap_mutex_);
  if (this->tap_active_) {
    this->tap_finishing_ = true;
    this->tap_finish_ms_ = millis();
  }
}

void GainSpeaker::drain_tap_() {
  const size_t frame_bytes = this->tap_info_.frames_to_bytes(1);
  for (int i = 0; i < 16 && this->tap_queue_.size(); ++i) {
    const size_t count = this->tap_queue_.contiguous_size();
    const size_t written = this->tap_->play(this->tap_queue_.front(), count, 0);
    if (!this->tap_queue_.consume(written, frame_bytes)) {
      this->tap_fault_.store(true);
      ESP_LOGE(TAG, "PCM tap returned an invalid frame count");
      return;
    }
    this->tap_accepted_bytes_.fetch_add(written);
    if (!written) break;
  }
}

void GainSpeaker::write_tap_(const uint8_t *data, size_t count, const audio::AudioStreamInfo &info) {
  if (this->tap_active_ && info != this->tap_info_) {
    ESP_LOGW(TAG, "PCM tap format changed; discarding old output");
    this->reset_tap_();
  }
  this->tap_info_ = info;
  this->tap_->set_audio_stream_info(info);
  this->tap_active_ = true;
  // Reclaim downstream space before deciding whether newly accepted PCM overflows.
  this->drain_tap_();
  if (this->tap_fault_.load()) return;
  const size_t inserted = this->tap_queue_.append(data, count, info.frames_to_bytes(1));
  this->tap_dropped_bytes_.fetch_add(count - inserted);
  this->drain_tap_();
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
  const uint32_t tap_epoch = this->tap_epoch_.load();
  const size_t written = write_scaled(
      data, length, frame_bytes, gain,
      [this, wait](const uint8_t *scaled, size_t count) { return this->output_->play(scaled, count, wait); },
      [this, &info, tap_epoch](const uint8_t *input, const uint8_t *scaled, size_t count) {
        if (this->pre_gain_tap_) this->pre_gain_tap_(input, count, info);
        {
          std::lock_guard<std::mutex> lock(this->tap_mutex_);
          if (this->tap_enabled_.load() && !this->tap_fault_.load() && !this->tap_suspended_ &&
              tap_epoch == this->tap_epoch_.load()) {
            if (this->ducked_.load() || (!this->fixed_ && this->muted_.load())) {
              int16_t silence[512]{};
              this->write_tap_(reinterpret_cast<const uint8_t *>(silence), count, info);
            } else {
              this->write_tap_(scaled, count, info);
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
  {
    std::lock_guard<std::mutex> lock(this->tap_mutex_);
    if (this->tap_finishing_) {
      this->drain_tap_();
      if (!this->tap_queue_.size()) {
        this->tap_finishing_ = false;
        this->tap_->finish();
      } else if (millis() - this->tap_finish_ms_ >= 10000) {
        ESP_LOGE(TAG, "PCM tap failed to drain within 10 seconds");
        this->tap_fault_.store(true);
        this->reset_tap_();
      }
    }
  }
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
