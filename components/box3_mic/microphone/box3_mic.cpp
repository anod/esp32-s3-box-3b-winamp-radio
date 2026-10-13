#include "box3_mic.h"
#include "../../internet_radio/media_player/voice_logic.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace esphome {
namespace box3_mic {

static const char *const TAG = "box3_mic";

void Box3Mic::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, 16000);
  this->stream_ = xStreamBufferCreate(STREAM_BYTES, 1);
  if (this->stream_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate stream buffer");
    this->mark_failed();
  }
}

void Box3Mic::dump_config() {
  ESP_LOGCONFIG(TAG, "BOX-3 Microphone:\n  Channel: %s\n  Format: 16 kHz mono 16-bit",
                this->use_right_ ? "right" : "left");
}

void Box3Mic::start() {
  if (this->is_failed()) return;
  this->want_run_ = true;
  if (this->radio_ != nullptr) this->radio_->voice_mic_requested(true);
  if (this->state_ == microphone::STATE_STOPPED) {
    this->state_ = microphone::STATE_STARTING;
  }
}

void Box3Mic::stop() {
  this->want_run_ = false;
  if (this->state_ == microphone::STATE_RUNNING) {
    this->state_ = microphone::STATE_STOPPING;
  }
}

void Box3Mic::loop() {
  switch (this->state_) {
    case microphone::STATE_STARTING: {
      if (!this->want_run_) {
        if (this->radio_ != nullptr) this->radio_->voice_mic_requested(false);
        this->state_ = microphone::STATE_STOPPED;
        break;
      }
      int begin = this->radio_ == nullptr ? 0 : this->radio_->voice_capture_begin();
      if (begin < 0) break;  // player still stopping; retry next loop
      if (begin == 0) {
        ESP_LOGE(TAG, "Failed to start I2S capture");
        if (this->radio_ != nullptr) {
          this->radio_->voice_capture_end();
          this->radio_->voice_mic_requested(false);
          this->radio_->voice_session_end();
        }
        this->want_run_ = false;
        this->state_ = microphone::STATE_STOPPED;
        break;
      }
      xStreamBufferReset(this->stream_);
      this->task_stop_ = false;
      this->task_done_ = false;
      this->dropped_bytes_ = 0;
      if (xTaskCreatePinnedToCore(capture_task_, "box3_mic", 4096, this, 5, &this->task_, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create capture task");
        this->task_done_ = true;
        this->radio_->voice_capture_end();
        this->radio_->voice_mic_requested(false);
        this->radio_->voice_session_end();
        this->want_run_ = false;
        this->state_ = microphone::STATE_STOPPED;
        break;
      }
      this->state_ = microphone::STATE_RUNNING;
      ESP_LOGD(TAG, "Capture started");
      break;
    }

    case microphone::STATE_RUNNING: {
      static std::vector<uint8_t> chunk;
      chunk.resize(1024);
      // Bounded drain per loop pass so a backlog can't starve the main loop
      for (int i = 0; i < 16; i++) {
        size_t n = xStreamBufferReceive(this->stream_, chunk.data(), chunk.size(), 0);
        if (n == 0) break;
        std::vector<uint8_t> data(chunk.begin(), chunk.begin() + n);
        this->data_callbacks_.call(data);
      }
      if (!this->want_run_) {
        this->state_ = microphone::STATE_STOPPING;
      }
      break;
    }

    case microphone::STATE_STOPPING:
      this->task_stop_ = true;
      if (this->task_done_) {
        this->finish_stop_();
      }
      break;

    default:
      break;
  }
}

void Box3Mic::finish_stop_() {
  this->task_ = nullptr;
  if (this->radio_ != nullptr) this->radio_->voice_capture_end();
  if (this->dropped_bytes_ > 0) {
    ESP_LOGW(TAG, "Dropped %lu bytes of mic audio", (unsigned long) this->dropped_bytes_);
  }
  if (!this->want_run_ && this->radio_ != nullptr) this->radio_->voice_mic_requested(false);
  internet_radio::voice_logic::g_mic_peak = 0;
  ESP_LOGD(TAG, "Capture stopped");
  this->state_ = this->want_run_ ? microphone::STATE_STARTING : microphone::STATE_STOPPED;
}

void Box3Mic::capture_task_(void *param) {
  auto *self = static_cast<Box3Mic *>(param);
  self->run_capture_();
  self->task_done_ = true;
  vTaskDelete(nullptr);
}

void Box3Mic::run_capture_() {
  i2s_chan_handle_t rx = this->radio_->get_i2s_rx();
  int16_t frames[FRAMES_PER_READ * 2];
  int16_t mono[FRAMES_PER_READ];
  const int pick = this->use_right_ ? 1 : 0;
  int peak[2] = {0, 0};
  int blocks = 0;

  while (!this->task_stop_) {
    size_t got = 0;
    esp_err_t err = i2s_channel_read(rx, frames, sizeof(frames), &got, 100);
    if (err != ESP_OK || got < 4) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    size_t n = got / 4;  // stereo 16-bit frames
    int block_peak[2] = {0, 0};
    internet_radio::voice_logic::extract_mono(frames, n, pick, mono, block_peak);
    for (int ch = 0; ch < 2; ch++)
      if (block_peak[ch] > peak[ch]) peak[ch] = block_peak[ch];
    int env = internet_radio::voice_logic::g_mic_peak;
    env -= env / 8;
    internet_radio::voice_logic::g_mic_peak = block_peak[pick] > env ? block_peak[pick] : env;
    size_t bytes = n * sizeof(int16_t);
    size_t sent = xStreamBufferSend(this->stream_, mono, bytes, 0);
    if (sent < bytes) this->dropped_bytes_ += bytes - sent;

    // ~1 s of capture between level reports, to pick the right mic channel
    if (++blocks >= 62) {
      ESP_LOGD(TAG, "Mic peak: left=%d right=%d", peak[0], peak[1]);
      peak[0] = peak[1] = 0;
      blocks = 0;
    }
  }
}

}  // namespace box3_mic
}  // namespace esphome
