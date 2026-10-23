#include "box3_mic.h"
#include "../../internet_radio/media_player/voice_logic.h"
#include "esphome/core/log.h"
#include "esp_attr.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace esphome {
namespace box3_mic {

static const char *const TAG = "box3_mic";

enum CapturePhase : uint32_t { CAPTURE_STOPPED, CAPTURE_READ, CAPTURE_RESAMPLE, CAPTURE_BUFFER };
static const char *const CAPTURE_PHASE_NAMES[] = {"stopped", "read", "resample", "buffer"};
static constexpr uint32_t CAPTURE_TRACE_MAGIC = 0x4D494331;

// Volatile stage markers survive panic resets even if the final log never reaches the API.
struct CaptureTrace {
  volatile uint32_t magic;
  volatile uint32_t wake;
  volatile uint32_t phase;
  volatile uint32_t uptime_ms;
  volatile uint32_t rate;
  volatile uint32_t frames;
  volatile uint32_t samples;
  volatile uint32_t stack_min;
  volatile uint32_t process_max_us;
};
static RTC_NOINIT_ATTR CaptureTrace capture_trace;
static CaptureTrace previous_capture_trace{};
static const Box3Mic *trace_owner = nullptr;

void Box3Mic::setup() {
  if (trace_owner == nullptr) {
    trace_owner = this;
    previous_capture_trace = capture_trace;
    capture_trace.magic = 0;
  }
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, 16000);
  this->stream_ = xStreamBufferCreate(STREAM_BYTES, 1);
  if (this->stream_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate stream buffer");
    this->mark_failed();
  }
}

void Box3Mic::dump_config() {
  ESP_LOGCONFIG(TAG, "BOX-3 Microphone:\n  Channel: %s\n  Mode: %s\n  Format: 16 kHz mono 16-bit",
                this->use_right_ ? "right" : "left", this->wake_mode_ ? "wake" : "session");
  if (trace_owner == this && previous_capture_trace.magic == CAPTURE_TRACE_MAGIC) {
    uint32_t phase = previous_capture_trace.phase;
    ESP_LOGW(TAG,
             "Previous mic trace: mode=%s phase=%s uptime_ms=%lu rate=%lu frames=%lu out=%lu "
             "stack_min=%lu process_max_us=%lu",
             previous_capture_trace.wake ? "wake" : "session",
             phase <= CAPTURE_BUFFER ? CAPTURE_PHASE_NAMES[phase] : "unknown",
             (unsigned long) previous_capture_trace.uptime_ms, (unsigned long) previous_capture_trace.rate,
             (unsigned long) previous_capture_trace.frames, (unsigned long) previous_capture_trace.samples,
             (unsigned long) previous_capture_trace.stack_min, (unsigned long) previous_capture_trace.process_max_us);
  }
}

void Box3Mic::start() {
  if (this->is_failed()) return;
  this->want_run_ = true;
  if (this->radio_ != nullptr && !this->wake_mode_) this->radio_->voice_mic_requested(true);
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
        if (this->radio_ != nullptr && !this->wake_mode_) this->radio_->voice_mic_requested(false);
        this->state_ = microphone::STATE_STOPPED;
        break;
      }
      if (rx_busy_) {
        // The other instance is still reading RX (wake task winding down)
        if (this->wait_start_ms_ == 0) this->wait_start_ms_ = millis();
        if (millis() - this->wait_start_ms_ < 3000) break;
        ESP_LOGE(TAG, "Capture path busy");
      }
      int begin = 0;
      if (!rx_busy_ && this->radio_ != nullptr)
        begin = this->wake_mode_ ? this->radio_->wake_capture_begin()
                                 : this->radio_->voice_capture_begin();
      if (begin < 0) break;  // player still stopping / session active; retry next loop
      this->wait_start_ms_ = 0;
      if (begin == 0 && this->wake_mode_) {
        // Keep retrying: the wake word component believes the mic is running
        if (!this->start_err_logged_) ESP_LOGE(TAG, "Failed to start wake capture, retrying");
        this->start_err_logged_ = true;
        break;
      }
      this->start_err_logged_ = false;
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
      rx_busy_ = true;
      this->dropped_bytes_ = 0;
      if (xTaskCreatePinnedToCore(capture_task_, "box3_mic", 8192, this, 5, &this->task_, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create capture task");
        this->task_done_ = true;
        rx_busy_ = false;
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
  rx_busy_ = false;
  if (this->radio_ != nullptr) this->radio_->voice_capture_end();
  if (this->dropped_bytes_ > 0) {
    ESP_LOGW(TAG, "Dropped %lu bytes of mic audio", (unsigned long) this->dropped_bytes_);
  }
  if (!this->wake_mode_) {
    if (!this->want_run_ && this->radio_ != nullptr) this->radio_->voice_mic_requested(false);
    internet_radio::voice_logic::g_mic_peak = 0;
  }
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
  // Out rate is 16 kHz; the bus runs at the radio's rate, so decimate (ratio 1 in a session)
  int16_t out[FRAMES_PER_READ];
  internet_radio::voice_logic::Decimator dec(16000);
  const int pick = this->use_right_ ? 1 : 0;
  int peak[2] = {0, 0};
  int blocks = 0;
  uint32_t last_diag_ms = millis();
  uint32_t timeouts = 0;
  uint32_t read_errors = 0;
  esp_err_t last_read_error = ESP_OK;
  uint32_t short_reads = 0;
  capture_trace.magic = 0;
  capture_trace.wake = this->wake_mode_;
  capture_trace.phase = CAPTURE_STOPPED;
  capture_trace.uptime_ms = last_diag_ms;
  capture_trace.rate = this->radio_->get_current_sample_rate();
  capture_trace.frames = 0;
  capture_trace.samples = 0;
  capture_trace.stack_min = uxTaskGetStackHighWaterMark(nullptr);
  capture_trace.process_max_us = 0;
  capture_trace.magic = CAPTURE_TRACE_MAGIC;

  while (!this->task_stop_) {
    uint32_t now = millis();
    if (now - last_diag_ms >= 5000) {
      capture_trace.stack_min = uxTaskGetStackHighWaterMark(nullptr);
      ESP_LOGD(TAG,
               "Mic diag: mode=%s rate=%lu frames=%lu out=%lu stack_min=%lu process_max_us=%lu "
               "timeouts=%lu read_err=%lu last_err=%s short=%lu buffered=%u dropped=%lu",
               this->wake_mode_ ? "wake" : "session", (unsigned long) capture_trace.rate,
               (unsigned long) capture_trace.frames, (unsigned long) capture_trace.samples,
               (unsigned long) capture_trace.stack_min, (unsigned long) capture_trace.process_max_us,
               (unsigned long) timeouts, (unsigned long) read_errors, esp_err_to_name(last_read_error),
               (unsigned long) short_reads, unsigned(xStreamBufferBytesAvailable(this->stream_)),
               (unsigned long) this->dropped_bytes_);
      last_diag_ms = now;
      timeouts = read_errors = short_reads = 0;
      last_read_error = ESP_OK;
      capture_trace.process_max_us = 0;
    }
    capture_trace.uptime_ms = now;
    capture_trace.phase = CAPTURE_READ;
    size_t got = 0;
    esp_err_t err = i2s_channel_read(rx, frames, sizeof(frames), &got, 100);
    if (err == ESP_ERR_TIMEOUT) {
      timeouts++;
    } else if (err != ESP_OK) {
      read_errors++;
      last_read_error = err;
    }
    if (err == ESP_OK && (got < 4 || got % 4 != 0)) short_reads++;
    if (err != ESP_OK || got < 4) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    size_t n = got / 4;  // stereo 16-bit frames
    int block_peak[2] = {0, 0};
    internet_radio::voice_logic::extract_mono(frames, n, pick, mono, block_peak);
    for (int ch = 0; ch < 2; ch++)
      if (block_peak[ch] > peak[ch]) peak[ch] = block_peak[ch];
    if (!this->wake_mode_) {
      int env = internet_radio::voice_logic::g_mic_peak;
      env -= env / 8;
      internet_radio::voice_logic::g_mic_peak = block_peak[pick] > env ? block_peak[pick] : env;
    }
    uint32_t rate = this->radio_->get_current_sample_rate();
    if (rate != capture_trace.rate) {
      ESP_LOGD(TAG, "Mic rate changed: mode=%s %lu -> %lu Hz", this->wake_mode_ ? "wake" : "session",
               (unsigned long) capture_trace.rate, (unsigned long) rate);
    }
    capture_trace.rate = rate;
    capture_trace.frames = n;
    capture_trace.samples = 0;
    capture_trace.phase = CAPTURE_RESAMPLE;
    uint32_t process_start = micros();
    size_t out_n = dec.process(rate, mono, n, out);
    uint32_t process_us = micros() - process_start;
    if (process_us > capture_trace.process_max_us) capture_trace.process_max_us = process_us;
    capture_trace.samples = out_n;
    capture_trace.phase = CAPTURE_BUFFER;
    size_t bytes = out_n * sizeof(int16_t);
    if (bytes > 0) {
      size_t sent = xStreamBufferSend(this->stream_, out, bytes, 0);
      if (sent < bytes) this->dropped_bytes_ += bytes - sent;
    }

    // ~1 s of capture between level reports, to pick the right mic channel
    if (++blocks >= 62) {
      if (!this->wake_mode_) ESP_LOGD(TAG, "Mic peak: left=%d right=%d", peak[0], peak[1]);
      peak[0] = peak[1] = 0;
      blocks = 0;
    }
  }
  capture_trace.uptime_ms = millis();
  capture_trace.phase = CAPTURE_STOPPED;
}

}  // namespace box3_mic
}  // namespace esphome
