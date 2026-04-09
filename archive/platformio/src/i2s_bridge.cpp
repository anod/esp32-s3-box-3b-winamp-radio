// ============================================================
// I2S1 Bridge — sends decoded PCM to WROOM-32D A2DP bridge
// Separate file to avoid Audio.h weak attribute propagation.
// ============================================================

#include <Arduino.h>
#include "driver/i2s_std.h"
#include "pins.h"

extern volatile bool btMode;

static i2s_chan_handle_t i2s1_tx = NULL;
static volatile uint32_t bridgeSampleRate = 44100;

void initBridgeI2S() {
    if (i2s1_tx) return;  // already initialized

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 480;  // 16×480=7680 frames (~174ms) — survives HTTPS/TLS stalls
    chan_cfg.auto_clear = true;    // send zeros when DMA empty (silence during gaps)
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s1_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S1_BCLK,
            .ws   = (gpio_num_t)I2S1_LRCK,
            .dout = (gpio_num_t)I2S1_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s1_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s1_tx));
    Serial.printf("I2S1 bridge: BCLK=%d LRCK=%d DOUT=%d  DMA=%d frames\n",
                  I2S1_BCLK, I2S1_LRCK, I2S1_DOUT, 16 * 480);
}

void deinitBridgeI2S() {
    i2s_chan_handle_t h = i2s1_tx;
    i2s1_tx = NULL;  // audio_process_i2s stops immediately
    if (h) {
        i2s_channel_disable(h);
        i2s_del_channel(h);
    }
    Serial.println("I2S1 bridge: deinitialized");
}

void updateBridgeSampleRate(uint32_t rate) {
    if (rate > 0 && rate != bridgeSampleRate) {
        Serial.printf("I2S1 bridge: resample %lu → 44100 Hz\n", rate);
        bridgeSampleRate = rate;
    }
}

// Overrides weak audio_process_i2s from ESP32-audioI2S library.
// Called after volume/gain/EQ, before I2S0 write. Runs on Core 0.
// I2S0 write MUST proceed (paces the decoder) — we piggyback on it.
void audio_process_i2s(int32_t* outBuff, int16_t validSamples, bool* continueI2S) {
    if (!btMode || !i2s1_tx || validSamples <= 0) return;
    if (validSamples > 2048) validSamples = 2048;

    static int16_t buf[2048 * 2];
    int n = validSamples * 2;
    for (int i = 0; i < n; i++) {
        buf[i] = (int16_t)(outBuff[i] >> 16);
    }

    size_t written = 0;
    uint32_t srcRate = bridgeSampleRate;

    if (srcRate != 44100 && srcRate > 0) {
        // Linear interpolation resampler: srcRate → 44100 Hz
        float step = (float)srcRate / 44100.0f;
        static int16_t resBuf[4096 * 2];
        int outIdx = 0;
        float phase = 0.0f;

        while (phase < validSamples - 1 && outIdx < 4096) {
            int si = (int)phase;
            float f = phase - si;
            resBuf[outIdx * 2]     = (int16_t)(buf[si * 2]     + f * (buf[(si + 1) * 2]     - buf[si * 2]));
            resBuf[outIdx * 2 + 1] = (int16_t)(buf[si * 2 + 1] + f * (buf[(si + 1) * 2 + 1] - buf[si * 2 + 1]));
            outIdx++;
            phase += step;
        }

        i2s_channel_write(i2s1_tx, resBuf, outIdx * 2 * sizeof(int16_t), &written, pdMS_TO_TICKS(10));
    } else {
        i2s_channel_write(i2s1_tx, buf, n * sizeof(int16_t), &written, pdMS_TO_TICKS(10));
    }
}
