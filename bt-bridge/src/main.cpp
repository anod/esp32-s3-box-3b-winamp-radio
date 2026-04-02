// ============================================================
// I2S→A2DP Bridge — ESP32-WROOM-32D
// Reads I2S slave from S3, pipes through ring buffer to A2DP.
// ============================================================

#include <Arduino.h>
#include "driver/i2s.h"
#include <BluetoothA2DPSource.h>
#include "ring_buffer.h"
#include "config.h"

// I2S slave RX pins (from S3)
#define I2S_BCLK   25
#define I2S_LRCK   26
#define I2S_DIN    27
#define SAMPLE_RATE 44100

static BluetoothA2DPSource a2dp;
static PcmRingBuffer       ringBuf(RING_BUF_FRAMES);

// Stats
static volatile uint32_t i2sFramesIn  = 0;
static volatile uint32_t a2dpFramesOut = 0;
static volatile uint32_t a2dpCallbacks = 0;
static unsigned long lastPrintMs = 0;

// ─── A2DP data callback (runs on BT task, Core 0) ────────

int32_t a2dpDataCallback(Frame* frame, int32_t frameCount) {
    a2dpCallbacks++;
    int read = ringBuf.read((int16_t*)frame, frameCount);
    a2dpFramesOut += read;
    return frameCount;  // ring buffer fills silence on underrun
}

// ─── I2S reader task (Core 1) ────────────────────────────

void i2sReaderTask(void* param) {
    const int BUF_FRAMES = 256;
    int16_t buf[BUF_FRAMES * 2];  // stereo

    for (;;) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(I2S_NUM_0, buf, sizeof(buf), &bytes_read, portMAX_DELAY);
        if (err == ESP_OK && bytes_read > 0) {
            int frames = bytes_read / (2 * sizeof(int16_t));
            ringBuf.write(buf, frames);
            i2sFramesIn += frames;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== I2S→A2DP Bridge Test ===");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_OFF);

    // --- I2S slave RX ---
    i2s_config_t i2s_cfg = {};
    i2s_cfg.mode            = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX);
    i2s_cfg.sample_rate     = SAMPLE_RATE;
    i2s_cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_cfg.channel_format  = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_cfg.dma_buf_count   = 8;
    i2s_cfg.dma_buf_len     = 256;
    i2s_cfg.use_apll        = false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = I2S_BCLK;
    pins.ws_io_num    = I2S_LRCK;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = I2S_DIN;

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
    Serial.printf("I2S RX: BCLK=%d  LRCK=%d  DIN=%d\n", I2S_BCLK, I2S_LRCK, I2S_DIN);

    // --- Start I2S reader on Core 1 ---
    xTaskCreatePinnedToCore(i2sReaderTask, "i2s_rx", 4096, NULL, 5, NULL, 1);

    // --- A2DP source (connects to JBL Flip 4) ---
    a2dp.set_auto_reconnect(true);
    a2dp.set_volume(80);
    a2dp.start(BT_SINK_NAME, a2dpDataCallback);
    Serial.printf("A2DP connecting to %s...\n", BT_SINK_NAME);

    digitalWrite(LED_PIN, LED_ON);
    lastPrintMs = millis();
}

void loop() {
    delay(100);

    unsigned long now = millis();
    if (now - lastPrintMs >= 3000) {
        unsigned long elapsed = now - lastPrintMs;
        uint32_t fin  = i2sFramesIn;
        uint32_t fout = a2dpFramesOut;
        uint32_t cb   = a2dpCallbacks;
        i2sFramesIn   = 0;
        a2dpFramesOut = 0;
        a2dpCallbacks = 0;

        Serial.printf("I2S in: %u f/s | A2DP out: %u f/s | callbacks: %u | buf: %u/%u | underruns: %u\n",
            (uint32_t)(fin  * 1000UL / elapsed),
            (uint32_t)(fout * 1000UL / elapsed),
            (uint32_t)(cb   * 1000UL / elapsed),
            ringBuf.available(), (uint32_t)RING_BUF_FRAMES,
            ringBuf.underruns());
        ringBuf.resetUnderruns();

        lastPrintMs = now;
    }
}
