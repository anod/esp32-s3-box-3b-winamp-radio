#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::gmf_audio {

enum class TransferResult { COMPLETE, CANCELLED, INVALID, STALLED };

template<typename Write, typename Active, typename Clock, typename Yield>
TransferResult drain_pcm(const uint8_t *data, size_t length, size_t frame_bytes,
                         Write write, Active active, Clock now, Yield yield) {
  if (frame_bytes == 0 || length == 0 || length % frame_bytes != 0) return TransferResult::INVALID;
  size_t position = 0;
  uint32_t progress = now();
  while (position < length) {
    if (!active()) return TransferResult::CANCELLED;
    const size_t count = write(data + position, length - position);
    if (count > length - position || count % frame_bytes != 0) return TransferResult::INVALID;
    position += count;
    if (count != 0) progress = now();
    else if (uint32_t(now() - progress) >= 10000) return TransferResult::STALLED;
    else yield();
  }
  return TransferResult::COMPLETE;
}

}  // namespace esphome::gmf_audio
