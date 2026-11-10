#include <algorithm>
#include <cassert>
#include <cmath>
#include "../../components/audio_spectrum/audio_spectrum.cpp"

int main() {
  spectrum_init();
  assert(!spectrum_compute());
  int16_t mono[128];
  int16_t stereo[256];
  for (int i = 0; i < 128; ++i) {
    mono[i] = int16_t(20000 * std::sin(6.283185307f * 8 * i / 128));
    stereo[i * 2] = stereo[i * 2 + 1] = mono[i];
  }
  feed_fft_samples(reinterpret_cast<uint8_t *>(stereo), sizeof(stereo));
  assert(spectrum_compute());
  float reference[16];
  std::copy(spec_bands, spec_bands + 16, reference);
  assert(*std::max_element(spec_bands, spec_bands + 16) > 10);
  feed_fft_samples(reinterpret_cast<uint8_t *>(mono), sizeof(mono), 1);
  assert(spectrum_compute());
  for (int i = 0; i < 16; ++i) assert(std::fabs(reference[i] - spec_bands[i]) < .001f);
  std::fill(mono, mono + 128, 0);
  feed_fft_samples(reinterpret_cast<uint8_t *>(mono), sizeof(mono), 1);
  assert(spectrum_compute());
  for (float band : spec_bands) assert(band == 0);
  assert(!spectrum_compute());
}
