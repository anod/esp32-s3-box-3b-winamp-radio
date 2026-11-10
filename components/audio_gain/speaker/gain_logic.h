#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::audio_gain {

inline int gain_q8(float volume) {
  if (!(volume > 0.0f)) return 0;
  return volume >= 1.0f ? 256 : static_cast<int>(volume * 256.0f + 0.5f);
}

// Each retry scales fresh input, never a previously scaled or partially consumed buffer.
template<typename Writer, typename Accepted>
size_t write_scaled(const uint8_t *input, size_t length, size_t frame_bytes, int gain,
                    Writer write, Accepted accepted) {
  if (frame_bytes == 0 || frame_bytes > 4 || length % frame_bytes != 0) return 0;
  int16_t scratch[512];
  const size_t count = std::min(length, sizeof(scratch)) / frame_bytes * frame_bytes;
  if (count == 0) return 0;
  memcpy(scratch, input, count);
  for (size_t i = 0; i < count / sizeof(int16_t); ++i) {
    const int32_t sample = static_cast<int32_t>(scratch[i]) * gain >> 8;
    scratch[i] = static_cast<int16_t>(std::clamp(sample, int32_t(INT16_MIN), int32_t(INT16_MAX)));
  }
  const size_t written = write(reinterpret_cast<const uint8_t *>(scratch), count);
  if (written > count || written % frame_bytes != 0) return SIZE_MAX;
  if (written != 0) accepted(input, reinterpret_cast<const uint8_t *>(scratch), written);
  return written;
}

}  // namespace esphome::audio_gain
