// ─── FFT spectrum analyser ────────────────────────────────
// Core 0 callback only accumulates mono samples (very lightweight).
// Core 1 runs the FFT in draw_frame_() — no audio-path overhead.
//
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

// ── Core 0 side: lightweight sample capture ──
// Double-buffered: Core 0 writes to buf[write_buf_], flips when full.
// Core 1 reads buf[1 - write_buf_] (the completed buffer).
static float sample_bufs[2][FFT_N];
static volatile int write_buf_ = 0;     // which buffer Core 0 is filling
static volatile int accum_idx = 0;
static volatile bool sample_ready = false;   // true when a full buffer is available
static volatile bool spectrum_ready = false; // init guard

// ── Core 1 side: FFT computation ──
static float fft_input[FFT_N * 2];    // interleaved Re/Im
static float fft_window[FFT_N];       // pre-computed Hann window
static float fft_twiddle[FFT_N];      // pre-computed twiddle factors (cos)
static float fft_twiddle_s[FFT_N];    // pre-computed twiddle factors (sin)

// Output bands: written by Core 1 in spectrum_compute(), read by Core 1 draw
float spec_bands[VIZ_BANDS] = {0};

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

// Call once before audio starts — pre-compute Hann window and twiddle factors.
void spectrum_init() {
  for (int i = 0; i < FFT_N; i++) {
    fft_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_N - 1)));
    float angle = -2.0f * (float)M_PI * i / FFT_N;
    fft_twiddle[i] = cosf(angle);
    fft_twiddle_s[i] = sinf(angle);
  }
  spectrum_ready = true;
}

// Called from Core 1 (draw_frame_) — runs FFT on latest completed sample buffer.
// Returns true if new data was computed.
bool spectrum_compute() {
  if (!sample_ready) return false;
  sample_ready = false;

  // Read the completed buffer (the one Core 0 is NOT writing to)
  int read_buf = 1 - write_buf_;
  for (int i = 0; i < FFT_N; i++) {
    fft_input[i * 2]     = sample_bufs[read_buf][i] * fft_window[i];
    fft_input[i * 2 + 1] = 0.0f;
  }

  fft_radix2(fft_input, FFT_N);

  // Aggregate FFT bins into 16 logarithmic frequency bands (magnitude²)
  for (int b = 0; b < VIZ_BANDS; b++) {
    float sum = 0.0f;
    for (int i = band_bin_start[b]; i <= band_bin_end[b]; i++) {
      float re = fft_input[i * 2];
      float im = fft_input[i * 2 + 1];
      sum += re * re + im * im;
    }
    int bin_count = band_bin_end[b] - band_bin_start[b] + 1;
    spec_bands[b] = sum / (float)bin_count;
  }
  return true;
}

// Strong override of the library's weak callback — STUB for ESP-IDF migration.
// The weak override pattern is removed (no ESP32-audioI2S library).
// Will be fed from PCM tap audio element in ESP-ADF (Step 3).
// Keeping the function signature for reference.
//
// void audio_process_raw_samples(int32_t *outBuff, int16_t validSamples) { ... }
