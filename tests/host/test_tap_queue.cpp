#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include "../../components/audio_gain/speaker/tap_queue.h"

using esphome::audio_gain::TapQueue;

int main() {
  uint8_t storage[16]{};
  const uint8_t input[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  TapQueue queue;
  assert(queue.append(input, 4, 4) == 0);
  queue.set_storage(storage, sizeof(storage));
  assert(queue.append(input, 3, 4) == 0);
  assert(queue.append(input, 12, 4) == 12);
  assert(queue.contiguous_size() == 12 && std::memcmp(queue.front(), input, 12) == 0);
  assert(!queue.consume(2, 4));
  assert(!queue.consume(16, 4));
  assert(queue.consume(8, 4));
  assert(queue.append(input + 12, 8, 4) == 8);
  assert(queue.size() == 12 && queue.contiguous_size() == 8);
  assert(std::memcmp(queue.front(), input + 8, 8) == 0);
  assert(queue.consume(8, 4));
  assert(queue.contiguous_size() == 4 && std::memcmp(queue.front(), input + 16, 4) == 0);
  assert(queue.append(input, 20, 4) == 12);
  assert(queue.size() == 16);
  assert(queue.append(input, 4, 4) == 0);
  queue.clear();
  assert(queue.size() == 0);
  assert(queue.append(input, 12, 2) == 12);
  assert(queue.consume(12, 2));
  assert(queue.append(input, 4, 0) == 0);
  queue.set_storage(storage, 15);
  assert(queue.append(input, 4, 4) == 0);

  queue.set_storage(storage, sizeof(storage));
  std::deque<uint8_t> expected;
  uint32_t random = 1;
  for (int step = 0; step < 10000; ++step) {
    random = random * 1664525u + 1013904223u;
    if (random & 1) {
      uint8_t bytes[12];
      for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = static_cast<uint8_t>(random + i);
      const size_t requested = ((random >> 8) % 4) * 4;
      const size_t inserted = queue.append(bytes, requested, 4);
      assert(inserted == std::min(requested, sizeof(storage) - expected.size()));
      for (size_t i = 0; i < inserted; ++i) expected.push_back(bytes[i]);
    } else {
      const size_t count = std::min(queue.contiguous_size(), size_t((random >> 8) % 4) * 4);
      for (size_t i = 0; i < count; ++i) {
        assert(queue.front()[i] == expected.front());
        expected.pop_front();
      }
      assert(queue.consume(count, 4));
    }
    assert(queue.size() == expected.size());
  }
}
