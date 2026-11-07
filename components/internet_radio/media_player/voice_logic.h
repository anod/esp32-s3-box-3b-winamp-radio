// Pure, hardware-free helpers for the voice assistant path.
// Kept header-only so they can be unit-tested on the host (tests/host).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace esphome {
namespace internet_radio {
namespace voice_logic {

// Decaying peak envelope of the selected mic channel (written by the capture task,
// read by the display). Exactly 0 means the mic delivers digital silence (hardware mute).
inline volatile int g_mic_peak = 0;

inline int playback_gain(int volume_gain, bool muted, bool announcing, int reply_gain) {
  return announcing ? reply_gain : (muted ? 0 : volume_gain);
}

// Two notes with a 5 ms fade at each edge, generated rather than stored as audio.
inline int16_t cue_sample(uint32_t frame, uint32_t rate, bool error) {
  const uint32_t note_frames = rate * (error ? 100 : 60) / 1000;
  if (note_frames == 0 || frame >= note_frames * 2) return 0;
  const uint32_t pos = frame % note_frames;
  const uint32_t fade = rate / 200;
  const uint32_t edge = pos < note_frames - 1 - pos ? pos : note_frames - 1 - pos;
  const float envelope = fade > 0 && edge < fade ? float(edge) / fade : 1.0f;
  const int hz = error ? (frame < note_frames ? 440 : 330) : (frame < note_frames ? 880 : 1175);
  return static_cast<int16_t>(4000 * envelope * std::sin(6.283185307f * hz * pos / rate));
}

// Map a 16-bit peak to a 0..100 meter on a log scale (half-octave steps).
// Idle room noise (~50) reads 0; normal speech reads roughly 55-85.
inline int level_pct(int peak) {
  if (peak < 2) return 0;
  int bits = 32 - __builtin_clz((unsigned) peak);
  int half = (peak >> (bits - 2)) & 1;
  int pct = (2 * bits + half - 13) * 100 / 17;
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

// Restore the radio only when nothing can still use the audio path.
inline bool should_restore(bool end_pending, bool announcing, bool announce_pending,
                           bool mic_requested, bool rx_enabled) {
  return end_pending && !announcing && !announce_pending && !mic_requested && !rx_enabled;
}

// Apply Q8 gain (256 = unity) with saturation, in place.
inline void apply_gain_q8(int16_t *samples, size_t count, int gain) {
  if (gain == 256) return;
  for (size_t i = 0; i < count; i++) {
    int32_t s = (int32_t) samples[i] * gain >> 8;
    if (s > INT16_MAX) s = INT16_MAX;
    if (s < INT16_MIN) s = INT16_MIN;
    samples[i] = (int16_t) s;
  }
}

inline uint32_t peak_abs(const int16_t *samples, size_t count) {
  uint32_t peak = 0;
  for (size_t i = 0; i < count; i++) {
    const int32_t v = samples[i];
    const uint32_t a = v < 0 ? uint32_t(-v) : uint32_t(v);
    if (a > peak) peak = a;
  }
  return peak;
}

// Extract one channel (0 = left, 1 = right) from interleaved stereo frames.
// Returns the peak absolute level of each channel through peak[2] (may be null).
inline void extract_mono(const int16_t *stereo, size_t frames, int pick, int16_t *mono,
                         int *peak) {
  for (size_t i = 0; i < frames; i++) {
    mono[i] = stereo[i * 2 + pick];
    if (peak) {
      for (int ch = 0; ch < 2; ch++) {
        int v = stereo[i * 2 + ch];
        if (v < 0) v = -v;
        if (v > peak[ch]) peak[ch] = v;
      }
    }
  }
}

// Box-filter (area-average) sample-rate converter to a lower rate, used to feed the
// 16 kHz wake word / assistant path from an I2S bus clocked at the radio's rate.
// Exact for in == out (copy). Resets when the input rate changes.
class Decimator {
 public:
  explicit Decimator(uint32_t out_rate) : out_(out_rate) {}

  // Convert n input samples at in_rate; returns samples written (max n * out / in + 1).
  size_t process(uint32_t in_rate, const int16_t *in, size_t n, int16_t *out) {
    if (in_rate == 0 || in_rate < out_) return 0;
    if (in_rate != in_) {
      in_ = in_rate;
      rem_ = in_rate;
      acc_ = 0;
    }
    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
      int64_t avail = out_;
      while (avail > 0) {
        int64_t take = avail < rem_ ? avail : rem_;
        acc_ += (int64_t) in[i] * take;
        avail -= take;
        rem_ -= take;
        if (rem_ == 0) {
          out[written++] = (int16_t) (acc_ / (int64_t) in_);
          acc_ = 0;
          rem_ = in_;
        }
      }
    }
    return written;
  }

 private:
  uint32_t out_;
  uint32_t in_{0};
  int64_t rem_{0};
  int64_t acc_{0};
};

}  // namespace voice_logic
}  // namespace internet_radio
}  // namespace esphome
