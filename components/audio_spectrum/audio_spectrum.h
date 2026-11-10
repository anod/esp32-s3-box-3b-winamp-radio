#pragma once

#include <cstdint>

extern float spec_bands[16];
void spectrum_init();
bool spectrum_compute();
void feed_fft_samples(const uint8_t *data, int size);
void feed_fft_samples(const uint8_t *data, int size, int channels);
