// Pure, hardware-free helpers for the voice assistant path.
// Kept header-only so they can be unit-tested on the host (tests/host).
#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace internet_radio {
namespace voice_logic {

// Decaying peak envelope of the selected mic channel (written by the capture task,
// read by the display). Exactly 0 means the mic delivers digital silence (hardware mute).
inline volatile int g_mic_peak = 0;

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

}  // namespace voice_logic
}  // namespace internet_radio
}  // namespace esphome
