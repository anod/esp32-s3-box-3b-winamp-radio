// ─── FFT spectrum analyser ────────────────────────────────
// Runs on Core 0 via the audio_process_raw_samples weak callback.
// This file must NOT include Audio.h to avoid inheriting the
// __attribute__((weak)) declaration, ensuring our symbol is strong.

#include <math.h>
#include <string.h>
#include <dsps_fft2r.h>
#include <dsps_wind_hann.h>
#include "spectrum.h"

// FFT working buffers
static float fftInput[FFT_N * 2];     // interleaved Re/Im for ESP-DSP
static float fftWindow[FFT_N];        // pre-computed Hann window
static float sampleAccum[FFT_N];      // mono sample accumulator
static int   accumIdx = 0;

// Shared output
volatile float specBands[VIZ_BANDS] = {0};

// Logarithmic bin mapping: 128-point FFT at 44100Hz → ~345Hz/bin
// Only map bins 1-45 (~345Hz–15.5kHz) — 128kbps MP3 has no content above ~16kHz
static const uint8_t bandBinStart[VIZ_BANDS] = { 1,  2,  3,  4,  5,  6,  7,  9, 11, 13, 16, 20, 24, 29, 35, 41};
static const uint8_t bandBinEnd[VIZ_BANDS]   = { 1,  2,  3,  4,  5,  6,  8, 10, 12, 15, 19, 23, 28, 34, 40, 48};

static void processFFT() {
    for (int i = 0; i < FFT_N; i++) {
        fftInput[i * 2]     = sampleAccum[i] * fftWindow[i];
        fftInput[i * 2 + 1] = 0.0f;
    }

    dsps_fft2r_fc32_ansi(fftInput, FFT_N);
    dsps_bit_rev_fc32_ansi(fftInput, FFT_N);

    for (int b = 0; b < VIZ_BANDS; b++) {
        float sum = 0.0f;
        for (int i = bandBinStart[b]; i <= bandBinEnd[b]; i++) {
            float re = fftInput[i * 2];
            float im = fftInput[i * 2 + 1];
            sum += sqrtf(re * re + im * im);
        }
        int binCount = bandBinEnd[b] - bandBinStart[b] + 1;
        specBands[b] = sum / (float)binCount;
    }
}

void spectrumInit() {
    dsps_fft2r_init_fc32(NULL, FFT_N);
    dsps_wind_hann_f32(fftWindow, FFT_N);
}

// Strong override of the library's weak callback (called on Core 0)
void audio_process_raw_samples(int32_t *outBuff, int16_t validSamples) {
    for (int i = 0; i < validSamples; i++) {
        // Mix stereo to mono, shift down (samples are 16-bit in int32)
        float l = (float)(outBuff[i * 2] >> 16);
        float r = (float)(outBuff[i * 2 + 1] >> 16);
        float mono = (l + r) * 0.5f;

        sampleAccum[accumIdx] = mono;
        accumIdx++;

        if (accumIdx >= FFT_N) {
            processFFT();
            accumIdx = 0;
        }
    }
}
