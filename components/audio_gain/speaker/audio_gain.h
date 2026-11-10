#pragma once

#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include <atomic>
#include <functional>
#include <mutex>

namespace esphome::audio_gain {

class GainSpeaker final : public Component, public speaker::Speaker {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void set_output_speaker(speaker::Speaker *output) { this->output_ = output; }
  void set_tap_speaker(speaker::Speaker *tap) { this->tap_ = tap; }
  void set_tap_enabled(bool enabled);
  bool is_tap_enabled() const { return this->tap_enabled_.load(); }
  using PcmTap = std::function<void(const uint8_t *, size_t, const audio::AudioStreamInfo &)>;
  void set_pre_gain_tap(PcmTap tap) { this->pre_gain_tap_ = std::move(tap); }
  void set_fixed_gain(float gain);
  void set_volume(float volume) override;
  float get_volume() override { return this->requested_volume_.load(); }
  void set_mute_state(bool muted) override { this->muted_.store(muted); }
  bool get_mute_state() override { return this->muted_.load(); }
  void set_ducked(bool ducked) { this->ducked_.store(ducked); }
  size_t play(const uint8_t *data, size_t length, TickType_t wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override { return this->output_->has_buffered_data(); }
  void set_pause_state(bool paused) override { this->output_->set_pause_state(paused); }
  bool get_pause_state() const override { return this->output_->get_pause_state(); }

 protected:
  speaker::Speaker *output_{nullptr};
  speaker::Speaker *tap_{nullptr};
  PcmTap pre_gain_tap_;
  std::mutex tap_mutex_;
  bool tap_active_{false};
  std::atomic<bool> tap_enabled_{false};
  std::atomic<bool> tap_fault_{false};
  std::atomic<uint32_t> tap_accepted_bytes_{0};
  std::atomic<uint32_t> tap_dropped_bytes_{0};
  bool fixed_{false};
  std::atomic<float> requested_volume_{1.0f};
  std::atomic<int> gain_{256};
  std::atomic<bool> muted_{false};
  std::atomic<bool> ducked_{false};
  std::atomic<uint32_t> accepted_bytes_{0};
  std::atomic<uint32_t> accepted_peak_{0};
  std::atomic<bool> output_fault_{false};
  uint32_t last_report_{0};
};

}  // namespace esphome::audio_gain
