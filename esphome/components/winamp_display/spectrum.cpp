// ─── FFT spectrum analyser ────────────────────────────────
// Runs on Core 0 via the audio_process_raw_samples weak callback.
// This file must NOT include Audio.h — the weak attribute would
// taint our strong definition (same rule as audio_process_i2s.cpp).
//
// Self-contained 128-point radix-2 FFT — no esp_dsp dependency.
// Uses only <math.h> and <string.h>.

#include <math.h>
#include <string.h>
#include <stdint.h>

static constexpr int FFT_N = 128;
static constexpr int VIZ_BANDS = 16;

// FFT working buffers (~2KB total, all internal RAM)
static float fft_input[FFT_N * 2];    // interleaved Re/Im
static float fft_window[FFT_N];       // pre-computed Hann window
static float fft_twiddle[FFT_N];      // pre-computed twiddle factors (cos)
static float fft_twiddle_s[FFT_N];    // pre-computed twiddle factors (sin)
static float sample_accum[FFT_N];     // mono sample accumulator
static int accum_idx = 0;
static volatile bool spectrum_ready = false;  // init guard

// Shared output: written by Core 0, read by Core 1
volatile float spec_bands[VIZ_BANDS] = {0};

// Logarithmic bin mapping: 128-point FFT at 44100Hz → ~345Hz/bin
// Maps bins 1-48 (~345Hz–16.5kHz)
static const uint8_t band_bin_start[VIZ_BANDS] = {1,  2,  3,  4,  5,  6,  7,  9, 11, 13, 16, 20, 24, 29, 35, 41};
static const uint8_t band_bin_end[VIZ_BANDS]   = {1,  2,  3,  4,  5,  6,  8, 10, 12, 15, 19, 23, 28, 34, 40, 48};

// In-place radix-2 decimation-in-time FFT on interleaved Re/Im float array.
static void fft_radix2(float *data, int n) {
  // Bit-reversal permutation
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float tr = data[i * 2];     float ti = data[i * 2 + 1];
      data[i * 2] = data[j * 2];  data[i * 2 + 1] = data[j * 2 + 1];
      data[j * 2] = tr;           data[j * 2 + 1] = ti;
    }
  }

  // Butterfly stages
  for (int len = 2; len <= n; len <<= 1) {
    int half = len >> 1;
    int step = n / len;
    for (int i = 0; i < n; i += len) {
      for (int j = 0; j < half; j++) {
        int tw = j * step;
        float wr = fft_twiddle[tw];
        float wi = fft_twiddle_s[tw];
        int e = (i + j) * 2;
        int o = (i + j + half) * 2;
        float tr = data[o] * wr - data[o + 1] * wi;
        float ti = data[o] * wi + data[o + 1] * wr;
        data[o]     = data[e]     - tr;
        data[o + 1] = data[e + 1] - ti;
        data[e]     += tr;
        data[e + 1] += ti;
      }
    }
  }
}

static void process_fft() {
  for (int i = 0; i < FFT_N; i++) {
    fft_input[i * 2]     = sample_accum[i] * fft_window[i];
    fft_input[i * 2 + 1] = 0.0f;
  }

  fft_radix2(fft_input, FFT_N);

  // Aggregate FFT bins into 16 logarithmic frequency bands
  for (int b = 0; b < VIZ_BANDS; b++) {
    float sum = 0.0f;
    for (int i = band_bin_start[b]; i <= band_bin_end[b]; i++) {
      float re = fft_input[i * 2];
      float im = fft_input[i * 2 + 1];
      sum += sqrtf(re * re + im * im);
    }
    int bin_count = band_bin_end[b] - band_bin_start[b] + 1;
    spec_bands[b] = sum / (float)bin_count;
  }
}

// Call once before audio starts — pre-compute Hann window and twiddle factors.
// Setup priority: WinampDisplay (LATE-2) runs after InternetRadio (LATE),
// but audio task won't stream until WiFi connects (checked in loop()).
// Guard with spectrum_ready flag for safety.
void spectrum_init() {
  for (int i = 0; i < FFT_N; i++) {
    fft_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_N - 1)));
    float angle = -2.0f * (float)M_PI * i / FFT_N;
    fft_twiddle[i] = cosf(angle);
    fft_twiddle_s[i] = sinf(angle);
  }
  spectrum_ready = true;
}

// Strong override of the library's weak callback (called on Core 0).
// C++ linkage to match Audio.h declaration — no extern "C".
void audio_process_raw_samples(int32_t *outBuff, int16_t validSamples) {
  if (!spectrum_ready) return;

  for (int i = 0; i < validSamples; i++) {
    // Mix stereo to mono — samples are 16-bit values in int32 containers
    float l = (float)(outBuff[i * 2] >> 16);
    float r = (float)(outBuff[i * 2 + 1] >> 16);
    sample_accum[accum_idx] = (l + r) * 0.5f;

    if (++accum_idx >= FFT_N) {
      process_fft();
      accum_idx = 0;
    }
  }
}
