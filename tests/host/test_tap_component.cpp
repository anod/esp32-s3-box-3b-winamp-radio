#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include "../../components/audio_gain/speaker/audio_gain.cpp"

uint32_t host_millis = 0;
static uint8_t queue_storage[128 * 1024];
void *heap_caps_malloc(size_t size, uint32_t) {
  assert(size == sizeof(queue_storage));
  return queue_storage;
}

class FakeSpeaker : public esphome::speaker::Speaker {
 public:
  bool blocked{false};
  bool invalid{false};
  int stops{0};
  int finishes{0};
  std::vector<uint8_t> bytes;
  std::function<void()> during_play;
  size_t play(const uint8_t *data, size_t length, TickType_t) override {
    if (during_play) during_play();
    if (invalid) return length + 4;
    if (blocked) return 0;
    bytes.insert(bytes.end(), data, data + length);
    state_ = esphome::speaker::STATE_RUNNING;
    return length;
  }
  size_t play(const uint8_t *data, size_t length) override { return play(data, length, 0); }
  void start() override { state_ = esphome::speaker::STATE_RUNNING; }
  void stop() override { ++stops; state_ = esphome::speaker::STATE_STOPPED; }
  void finish() override { ++finishes; state_ = esphome::speaker::STATE_STOPPED; }
  bool has_buffered_data() const override { return false; }
};

int main() {
  using esphome::audio::AudioStreamInfo;
  esphome::audio_gain::GainSpeaker gain;
  FakeSpeaker output, tap;
  gain.set_output_speaker(&output);
  gain.set_tap_speaker(&tap);
  gain.set_tap_enabled(false);
  assert(tap.stops == 0);
  gain.setup();
  gain.set_audio_stream_info(AudioStreamInfo(16, 2, 44100));
  gain.set_tap_enabled(true);
  const int16_t input[] = {1000, -1000, 2000, -2000};
  const auto *pcm = reinterpret_cast<const uint8_t *>(input);
  tap.blocked = true;
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  assert(tap.bytes.empty());
  tap.blocked = false;
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  assert(tap.bytes.size() == sizeof(input) * 2);
  assert(std::memcmp(tap.bytes.data(), input, sizeof(input)) == 0);
  assert(std::memcmp(tap.bytes.data() + sizeof(input), input, sizeof(input)) == 0);

  tap.blocked = true;
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  gain.finish();
  gain.loop();
  assert(tap.finishes == 0);
  tap.blocked = false;
  gain.loop();
  assert(tap.finishes == 1 && tap.bytes.size() == sizeof(input) * 3);

  gain.set_tap_suspended(true);
  const size_t before = tap.bytes.size();
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  assert(tap.bytes.size() == before);
  gain.set_tap_suspended(false);
  output.during_play = [&gain] {
    gain.set_tap_suspended(true);
    gain.set_tap_suspended(false);
  };
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  assert(tap.bytes.size() == before);
  output.during_play = nullptr;

  gain.set_ducked(true);
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  for (size_t i = before; i < tap.bytes.size(); ++i) assert(tap.bytes[i] == 0);
  gain.set_ducked(false);
  gain.set_mute_state(true);
  const size_t muted_before = tap.bytes.size();
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  for (size_t i = muted_before; i < tap.bytes.size(); ++i) assert(tap.bytes[i] == 0);
  gain.set_mute_state(false);

  tap.blocked = true;
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  const int stops_before_format = tap.stops;
  gain.set_audio_stream_info(AudioStreamInfo(16, 2, 48000));
  tap.blocked = false;
  const size_t format_before = tap.bytes.size();
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  assert(tap.stops == stops_before_format + 1);
  assert(tap.bytes.size() == format_before + sizeof(input));
  assert(std::memcmp(tap.bytes.data() + format_before, input, sizeof(input)) == 0);

  tap.invalid = true;
  assert(gain.play(pcm, sizeof(input), 0) == sizeof(input));
  gain.loop();
  assert(!gain.is_tap_enabled());

  esphome::audio_gain::GainSpeaker full;
  FakeSpeaker full_output, full_tap;
  full.set_output_speaker(&full_output);
  full.set_tap_speaker(&full_tap);
  full.setup();
  full.set_audio_stream_info(AudioStreamInfo(16, 2, 44100));
  full.set_tap_enabled(true);
  full_tap.blocked = true;
  uint8_t block[1024]{};
  for (int i = 0; i < 128; ++i) assert(full.play(block, sizeof(block), 0) == sizeof(block));
  full_tap.blocked = false;
  assert(full.play(block, sizeof(block), 0) == sizeof(block));
  assert(full_tap.bytes.size() == 129 * sizeof(block));

  esphome::audio_gain::GainSpeaker stalled;
  FakeSpeaker stalled_output, stalled_tap;
  stalled.set_output_speaker(&stalled_output);
  stalled.set_tap_speaker(&stalled_tap);
  stalled.setup();
  stalled.set_audio_stream_info(AudioStreamInfo(16, 2, 44100));
  stalled.set_tap_enabled(true);
  stalled_tap.blocked = true;
  assert(stalled.play(pcm, sizeof(input), 0) == sizeof(input));
  stalled.finish();
  host_millis = 9999;
  stalled.loop();
  assert(stalled.is_tap_enabled());
  host_millis = 10000;
  stalled.loop();
  assert(!stalled.is_tap_enabled() && stalled_tap.stops == 1);
}
