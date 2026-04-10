// ============================================================
// audio_process_i2s weak override — MUST be in separate TU
// This file must NOT include Audio.h — weak attribute taints
// any definition in the same translation unit.
// ============================================================

#include <Arduino.h>
#include "driver/i2s_std.h"
#include "i2s_bridge.h"

using esphome::i2s_bridge::I2SBridge;

// Frame counter for buffer underrun watchdog (defined in internet_radio.cpp)
extern volatile uint32_t g_audio_frame_count;

// Overrides weak audio_process_i2s from ESP32-audioI2S library.
// Called after volume/gain/EQ, before I2S0 write. Runs on Core 0.
// I2S0 write MUST proceed (paces the decoder) — we piggyback on it.
void audio_process_i2s(int32_t *outBuff, int16_t validSamples, bool *continueI2S) {
  g_audio_frame_count++;  // near-zero-cost heartbeat for underrun watchdog

  if (!I2SBridge::is_active() || validSamples <= 0) return;

  i2s_chan_handle_t tx = I2SBridge::get_tx_handle();
  if (!tx) return;

  if (validSamples > 2048) validSamples = 2048;

  static int16_t buf[2048 * 2];
  int n = validSamples * 2;
  for (int i = 0; i < n; i++) {
    buf[i] = (int16_t)(outBuff[i] >> 16);
  }

  size_t written = 0;
  uint32_t srcRate = I2SBridge::bridge_sample_rate;

  if (srcRate != 44100 && srcRate > 0) {
    // Linear interpolation resampler: srcRate → 44100 Hz
    float step = (float)srcRate / 44100.0f;
    static int16_t resBuf[4096 * 2];
    int outIdx = 0;
    float phase = 0.0f;

    while (phase < validSamples - 1 && outIdx < 4096) {
      int si = (int)phase;
      float f = phase - si;
      resBuf[outIdx * 2] = (int16_t)(buf[si * 2] + f * (buf[(si + 1) * 2] - buf[si * 2]));
      resBuf[outIdx * 2 + 1] = (int16_t)(buf[si * 2 + 1] + f * (buf[(si + 1) * 2 + 1] - buf[si * 2 + 1]));
      outIdx++;
      phase += step;
    }

    i2s_channel_write(tx, resBuf, outIdx * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(10));
  } else {
    i2s_channel_write(tx, buf, n * sizeof(int16_t), &written, pdMS_TO_TICKS(10));
  }
}
