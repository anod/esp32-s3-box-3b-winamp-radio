#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

using TickType_t = unsigned;

namespace esphome::audio {
class AudioStreamInfo {
 public:
  AudioStreamInfo(uint8_t bits = 16, uint8_t channels = 1, uint32_t rate = 16000)
      : bits_(bits), channels_(channels), rate_(rate) {}
  uint8_t get_bits_per_sample() const { return bits_; }
  uint8_t get_channels() const { return channels_; }
  size_t frames_to_bytes(size_t frames) const { return frames * channels_ * bits_ / 8; }
  bool operator!=(const AudioStreamInfo &other) const {
    return bits_ != other.bits_ || channels_ != other.channels_ || rate_ != other.rate_;
  }
 private:
  uint8_t bits_, channels_;
  uint32_t rate_;
};
}  // namespace esphome::audio

namespace esphome::speaker {
enum State { STATE_STOPPED, STATE_STARTING, STATE_RUNNING };
template<typename Signature> struct Callbacks;
template<typename... Args> struct Callbacks<void(Args...)> {
  std::function<void(Args...)> callback;
  void call(Args... args) { if (callback) callback(args...); }
};
class Speaker {
 public:
  virtual ~Speaker() = default;
  virtual size_t play(const uint8_t *, size_t, TickType_t) = 0;
  virtual size_t play(const uint8_t *, size_t) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void finish() = 0;
  virtual bool has_buffered_data() const = 0;
  virtual void set_pause_state(bool) {}
  virtual bool get_pause_state() const { return false; }
  virtual void set_volume(float) {}
  virtual float get_volume() { return 1; }
  virtual void set_mute_state(bool) {}
  virtual bool get_mute_state() { return false; }
  void set_audio_stream_info(const audio::AudioStreamInfo &info) { audio_stream_info_ = info; }
  template<typename F> void add_audio_output_callback(F callback) { audio_output_callback_.callback = callback; }
  bool is_stopped() const { return state_ == STATE_STOPPED; }
  bool is_running() const { return state_ == STATE_RUNNING; }
 protected:
  State state_{STATE_STOPPED};
  audio::AudioStreamInfo audio_stream_info_;
  Callbacks<void(uint32_t, int64_t)> audio_output_callback_;
};
}  // namespace esphome::speaker
