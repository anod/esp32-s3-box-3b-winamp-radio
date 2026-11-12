#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::audio_gain {

// Caller serializes access and provides storage aligned to complete PCM frames.
class TapQueue {
 public:
  void set_storage(uint8_t *storage, size_t capacity) {
    this->storage_ = storage;
    this->capacity_ = capacity;
    this->clear();
  }
  size_t size() const { return this->size_; }
  size_t capacity() const { return this->capacity_; }
  void clear() { this->head_ = this->size_ = 0; }

  size_t append(const uint8_t *data, size_t length, size_t frame_bytes) {
    if (!this->storage_ || frame_bytes == 0 || length % frame_bytes != 0 ||
        this->capacity_ % frame_bytes != 0)
      return 0;
    const size_t count = std::min(length, this->capacity_ - this->size_) / frame_bytes * frame_bytes;
    const size_t tail = (this->head_ + this->size_) % this->capacity_;
    const size_t first = std::min(count, this->capacity_ - tail);
    std::memcpy(this->storage_ + tail, data, first);
    std::memcpy(this->storage_, data + first, count - first);
    this->size_ += count;
    return count;
  }

  const uint8_t *front() const { return this->storage_ + this->head_; }
  size_t contiguous_size() const { return std::min(this->size_, this->capacity_ - this->head_); }
  bool consume(size_t count, size_t frame_bytes) {
    if (frame_bytes == 0 || count > this->contiguous_size() || count % frame_bytes != 0) return false;
    if (count != 0) {
      this->head_ = (this->head_ + count) % this->capacity_;
      this->size_ -= count;
    }
    return true;
  }

 private:
  uint8_t *storage_{nullptr};
  size_t capacity_{0};
  size_t head_{0};
  size_t size_{0};
};

}  // namespace esphome::audio_gain
