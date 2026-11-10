#include <cassert>
#include <cstring>
#include <limits>
#include <vector>
#include "../../components/audio_gain/speaker/gain_logic.h"

int main() {
  using namespace esphome::audio_gain;
  assert(gain_q8(0) == 0 && gain_q8(1) == 256 && gain_q8(.5f) == 128);
  assert(gain_q8(std::numeric_limits<float>::quiet_NaN()) == 0);
  const int16_t samples[] = {32767, -32768, 12000, -12000, 256, -256, 1, -1};
  std::vector<uint8_t> original(sizeof(samples) + 1);
  memcpy(original.data() + 1, samples, sizeof(samples));
  std::vector<uint8_t> output;
  std::vector<uint8_t> tapped;
  std::vector<uint8_t> pre_gain;
  size_t calls = 0;
  auto writer = [&](const uint8_t *data, size_t length) {
    const size_t count = calls++ == 0 ? 0 : std::min(length, size_t(4));
    output.insert(output.end(), data, data + count);
    return count;
  };
  auto accepted = [&](const uint8_t *input, const uint8_t *scaled, size_t length) {
    pre_gain.insert(pre_gain.end(), input, input + length);
    tapped.insert(tapped.end(), scaled, scaled + length);
  };
  size_t position = 0;
  while (position < sizeof(samples)) {
    position += write_scaled(original.data() + 1 + position, sizeof(samples) - position, 4, 128,
                             writer, accepted);
  }
  assert(calls == 5);
  assert(output == tapped);
  assert(pre_gain.size() == sizeof(samples));
  assert(memcmp(pre_gain.data(), samples, sizeof(samples)) == 0);
  assert(memcmp(original.data() + 1, samples, sizeof(samples)) == 0);
  int16_t result[8];
  memcpy(result, output.data(), sizeof(result));
  for (size_t i = 0; i < 8; ++i) assert(result[i] == (int32_t(samples[i]) * 128 >> 8));
  auto all = [](const uint8_t *, size_t length) { return length; };
  auto silent = [](const uint8_t *, const uint8_t *scaled, size_t length) {
    for (size_t i = 0; i < length; ++i) assert(scaled[i] == 0);
  };
  assert(write_scaled(original.data() + 1, sizeof(samples), 4, 0, all, silent) == sizeof(samples));
  auto no_tap = [](const uint8_t *, const uint8_t *, size_t) { assert(false); };
  assert(write_scaled(original.data(), 3, 4, 256, all, no_tap) == 0);
  assert(write_scaled(original.data(), 4, 4, 256,
                      [](const uint8_t *, size_t) { return size_t(2); }, no_tap) == SIZE_MAX);
  assert(write_scaled(original.data(), 4, 4, 256,
                      [](const uint8_t *, size_t) { return size_t(8); }, no_tap) == SIZE_MAX);
  std::vector<uint8_t> large(4096, 0);
  assert(write_scaled(large.data(), large.size(), 4, 256, all, silent) == 1024);
}
