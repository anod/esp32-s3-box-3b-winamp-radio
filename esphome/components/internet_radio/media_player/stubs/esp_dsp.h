// Stub esp_dsp.h — satisfies ESP32-audioI2S #include without pulling in the
// real espressif/esp-dsp IDF component. We don't use EQ or FFT features.
// If you later need setTone() or getVULevel(), replace this with the real lib.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// FFT stubs (used by Audio::computeVUlevel / spectrum analyzer)
static inline esp_err_t dsps_fft2r_init_fc32(float *table, int N) { return ESP_OK; }
static inline esp_err_t dsps_fft2r_fc32(float *data, int N) { return ESP_OK; }
static inline esp_err_t dsps_bit_rev_fc32(float *data, int N) { return ESP_OK; }
static inline esp_err_t dsps_cplx2reC_fc32(float *data, int N) { return ESP_OK; }

// Biquad filter stubs (used by Audio::setTone for 3-band EQ)
static inline esp_err_t dsps_biquad_gen_lowShelf_f32(float *c, float freq, float gain, float Q) { return ESP_OK; }
static inline esp_err_t dsps_biquad_gen_highShelf_f32(float *c, float freq, float gain, float Q) { return ESP_OK; }
static inline esp_err_t dsps_biquad_sf32(const float *input, float *output, int len, float *coef, float *w) { return ESP_OK; }

#ifdef __cplusplus
}
#endif
