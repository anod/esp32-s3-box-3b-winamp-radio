// ============================================================
// audio_process_i2s — LEGACY STUB
// Frame counting and I2S bridge writes are now handled in the
// ESP-GMF pcm_output_cb_ (internet_radio.cpp).
// This file kept for g_audio_frame_count extern declaration.
// ============================================================

#include <cstdint>

// Frame counter for buffer underrun watchdog (defined in internet_radio.cpp)
extern volatile uint32_t g_audio_frame_count;
