// ============================================================
// audio_process_i2s — STUB for ESP-IDF migration
// The weak override pattern is removed (no ESP32-audioI2S library).
// Will be replaced by a PCM tap audio element in ESP-ADF (Step 3).
// ============================================================

#include <cstdint>

// Frame counter for buffer underrun watchdog (defined in internet_radio.cpp)
extern volatile uint32_t g_audio_frame_count;
