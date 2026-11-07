#include <cassert>
#include <cstdint>
#include "../../components/internet_radio/media_player/voice_logic.h"

int main() {
  using esphome::internet_radio::voice_logic::playback_gain;
  assert(playback_gain(128, false, false, 192) == 128);
  assert(playback_gain(128, true, false, 192) == 0);
  assert(playback_gain(256, true, false, 192) == 0);
  assert(playback_gain(256, false, false, 192) == 256);
  assert(playback_gain(0, false, false, 192) == 0);
  assert(playback_gain(0, true, true, 192) == 192);
  for (int gain = 0; gain <= 256; ++gain) {
    assert(playback_gain(gain, true, false, 192) == 0);
    assert(playback_gain(gain, false, false, 192) == gain);
    assert(playback_gain(gain, true, true, 192) == 192);
    assert(playback_gain(gain, false, true, 192) == 192);
  }
  using esphome::internet_radio::voice_logic::peak_abs;
  const int16_t samples[] = {0, 12, -300, INT16_MIN, 32767};
  assert(peak_abs(samples, 0) == 0);
  assert(peak_abs(samples, 3) == 300);
  assert(peak_abs(samples, 5) == 32768);
}
