#pragma once
#include <stdint.h>

static constexpr int FFT_N     = 128;
static constexpr int VIZ_BANDS = 16;

// Shared spectrum data: written by Core 0, read by Core 1
extern volatile float specBands[VIZ_BANDS];

// Call once in setup() before audio starts
void spectrumInit();
