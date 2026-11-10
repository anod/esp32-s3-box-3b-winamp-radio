#include <cassert>
#include <cstring>
#include <vector>
#include "../../components/gmf_audio/pcm_transfer.h"

int main() {
  using namespace esphome::gmf_audio;
  uint8_t data[64];
  for (size_t i = 0; i < sizeof(data); ++i) data[i] = i;
  std::vector<uint8_t> received;
  uint32_t clock = 0;
  size_t calls = 0;
  auto sink = [&](const uint8_t *input, size_t length) {
    const size_t count = calls++ % 3 == 0 ? 0 : (length > 8 ? 8 : length);
    received.insert(received.end(), input, input + count);
    return count;
  };
  auto active = []() { return true; };
  auto now = [&]() { return clock; };
  auto yield = [&]() { clock += 20; };
  assert(drain_pcm(data, sizeof(data), 4, sink, active, now, yield) == TransferResult::COMPLETE);
  assert(received.size() == sizeof(data) && memcmp(data, received.data(), sizeof(data)) == 0);
  auto blocked = [](const uint8_t *, size_t) { return size_t(0); };
  clock = 0;
  assert(drain_pcm(data, sizeof(data), 4, blocked, [&]() { return clock < 200; }, now, yield) ==
         TransferResult::CANCELLED);
  assert(clock == 200);
  clock = 0;
  assert(drain_pcm(data, sizeof(data), 4, blocked, active, now, yield) == TransferResult::STALLED);
  assert(clock == 10000);
  clock = UINT32_MAX - 100;
  assert(drain_pcm(data, sizeof(data), 4, blocked, active, now, yield) == TransferResult::STALLED);
  assert(drain_pcm(data, sizeof(data), 4, [](const uint8_t *, size_t) { return size_t(2); },
                   active, now, yield) == TransferResult::INVALID);
  assert(drain_pcm(data, sizeof(data), 4, [](const uint8_t *, size_t n) { return n + 4; },
                   active, now, yield) == TransferResult::INVALID);
  assert(drain_pcm(data, 3, 4, sink, active, now, yield) == TransferResult::INVALID);
}
