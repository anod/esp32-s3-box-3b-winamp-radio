#include "gmf_media_source.h"

#ifdef USE_GMF_MEDIA_SOURCE
#include "playback_logic.h"
#include "pcm_transfer.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esp_crt_bundle.h"
#include <cstring>

namespace esphome::gmf_audio {

void initialize_codec_memory();
static const char *const TAG = "gmf_source";

void GmfMediaSource::setup() {
  initialize_codec_memory();
  esp_asp_cfg_t config{};
  config.out.cb = pcm_;
  config.out.user_ctx = this;
  config.task_prio = 5;
  config.task_stack = 8192;
  config.task_core = 0;
  config.task_stack_in_ext = true;
  if (esp_audio_simple_player_new(&config, &this->player_) != ESP_GMF_ERR_OK || !this->player_) {
    ESP_LOGE(TAG, "Failed to create GMF player");
    this->mark_failed();
    return;
  }
  esp_audio_simple_player_set_event(this->player_, event_, this);
  http_io_cfg_t http_config = HTTP_STREAM_CFG_DEFAULT();
  http_config.dir = ESP_GMF_IO_DIR_READER;
  http_config.crt_bundle_attach = esp_crt_bundle_attach;
  http_config.event_handle = HttpTransport::event;
  http_config.user_data = &this->transport_;
  http_config.io_cfg.buffer_cfg.buffer_size = 256 * 1024;
  if (esp_gmf_io_http_init(&http_config, &this->http_) != ESP_GMF_ERR_OK ||
      esp_audio_simple_player_register_io(this->player_, this->http_) != ESP_GMF_ERR_OK) {
    ESP_LOGE(TAG, "Failed to register cancellable HTTP transport");
    this->mark_failed();
    return;
  }
  this->commands_ = xQueueCreate(1, sizeof(Command));
  if (!this->commands_ ||
      xTaskCreatePinnedToCore(control_task_, "gmf_control", 6144, this, 6, nullptr, 0) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create GMF control worker");
    this->mark_failed();
  }
}

bool GmfMediaSource::can_handle(const std::string &uri) const {
  return uri.compare(0, 7, "http://") == 0 || uri.compare(0, 8, "https://") == 0;
}

bool GmfMediaSource::play_uri(const std::string &uri) {
  if (!this->can_handle(uri) || !this->has_listener() || this->is_failed() || this->pending_() ||
      this->get_state() != media_source::MediaSourceState::IDLE) {
    ESP_LOGE(TAG, "URI rejected: source unavailable, busy, or unsupported scheme");
    return false;
  }
  if (!this->request_(Operation::RUN, uri.c_str())) return false;
  if (this->title_sensor_) this->title_sensor_->publish_state("");
  // This is readiness for SourceBinding, not evidence that PCM reached the DAC.
  this->set_state_(media_source::MediaSourceState::PLAYING);
  this->phase_("connecting");
  return true;
}

void GmfMediaSource::handle_command(media_source::MediaSourceCommand command) {
  using media_source::MediaSourceCommand;
  switch (command) {
    case MediaSourceCommand::STOP:
      if ((this->get_state() != media_source::MediaSourceState::IDLE || this->pending_()) &&
          this->request_(Operation::STOP))
        this->phase_("stopping");
      break;
    case MediaSourceCommand::PAUSE:
      if (this->get_state() == media_source::MediaSourceState::PLAYING && this->request_(Operation::PAUSE)) {
        this->set_state_(media_source::MediaSourceState::PAUSED);
        this->phase_("paused");
      }
      break;
    case MediaSourceCommand::PLAY:
      if (this->get_state() == media_source::MediaSourceState::PAUSED && this->request_(Operation::RESUME)) {
        this->set_state_(media_source::MediaSourceState::PLAYING);
        this->phase_("connecting");
      }
      break;
    default:
      break;
  }
}

bool GmfMediaSource::request_(Operation operation, const char *uri) {
  std::lock_guard<std::mutex> lock(this->request_mutex_);
  if (this->is_failed() || !this->commands_ || this->quarantined_.load()) {
    ESP_LOGE(TAG, "Control request rejected: GMF unavailable or quarantined");
    return false;
  }
  Command command{};
  command.operation = operation;
  if (uri) {
    if (strlen(uri) >= sizeof(command.uri) - 6) {
      ESP_LOGE(TAG, "URI too long");
      return false;
    }
    const char *extension = strrchr(uri, '.');
    if (!extension || strchr(extension, '/'))
      snprintf(command.uri, sizeof(command.uri), "%s#.aac", uri);
    else
      strlcpy(command.uri, uri, sizeof(command.uri));
  }
  if (!this->pending_()) this->command_ms_ = millis();
  command.generation = this->generation_.fetch_add(1) + 1;
  if (operation == Operation::RUN || operation == Operation::STOP) {
    this->accept_.store(false);
    this->pcm_enabled_.store(false);
    command.epoch = this->epoch_.fetch_add(1) + 1;
    this->transport_.set_epoch(command.epoch);
    this->finished_.store(false);
    this->failed_.store(false);
    this->launched_.store(false);
    this->played_frames_.store(0);
  } else if (operation == Operation::PAUSE) {
    this->pcm_enabled_.store(false);
  } else if (operation == Operation::RESUME) {
    this->played_frames_.store(0);
  }
  return xQueueOverwrite(this->commands_, &command) == pdPASS;
}

bool GmfMediaSource::settle_() {
  if (!this->settle_pending_.exchange(false)) return true;
  const auto task = this->decoder_task_.load();
  if (!task) return true;
  const uint32_t start = millis();
  int parked = 0;
  while (parked < 3 && millis() - start < 500) {
    const auto state = eTaskGetState(task);
    parked = state == eBlocked || state == eSuspended ? parked + 1 : 0;
    if (parked < 3) vTaskDelay(1);
  }
  if (parked < 3) ESP_LOGE(TAG, "GMF task did not park after a terminal event");
  return parked == 3;
}

esp_gmf_err_t GmfMediaSource::stop_(bool launched) {
  auto result = esp_audio_simple_player_stop(this->player_);
  if (result == ESP_GMF_ERR_OK && launched && !this->ended_.load()) {
    ESP_LOGW(TAG, "Stale GMF stop bit; waiting for a terminal event");
    result = esp_audio_simple_player_stop(this->player_);
    if (result == ESP_GMF_ERR_OK && !this->ended_.load()) {
      ESP_LOGE(TAG, "GMF stop not confirmed");
      result = ESP_GMF_ERR_TIMEOUT;
    }
  }
  return result;
}

void GmfMediaSource::control_task_(void *argument) {
  auto *self = static_cast<GmfMediaSource *>(argument);
  playback_logic::PipelineStopTracker tracker;
  bool launched = false;
  Command command{};
  for (;;) {
    if (xQueueReceive(self->commands_, &command, portMAX_DELAY) != pdTRUE) continue;
    const uint32_t start = millis();
    auto result = ESP_GMF_ERR_OK;
    if (command.operation == Operation::STOP || command.operation == Operation::RUN) {
      self->accept_.store(false);
      if (!self->settle_()) result = ESP_GMF_ERR_TIMEOUT;
      if (tracker.needs_stop()) {
        const auto stopped = self->stop_(launched);
        tracker.stop_result(stopped == ESP_GMF_ERR_OK);
        launched = false;
        if (stopped != ESP_GMF_ERR_OK) result = stopped;
      }
      bool run = false;
      {
        std::lock_guard<std::mutex> lock(self->request_mutex_);
        if (result == ESP_GMF_ERR_OK && command.generation == self->generation_.load() &&
            !self->quarantined_.load()) {
          if (command.operation == Operation::RUN) {
            self->transport_.launch(command.epoch);
            self->accept_.store(true);
            self->pcm_enabled_.store(true);
            self->ended_.store(false);
            self->launch_ms_.store(millis());
            self->launched_.store(true);
            run = true;
          } else {
            self->finished_.store(true);
          }
        }
      }
      if (run) {
        result = esp_audio_simple_player_run(self->player_, command.uri, nullptr);
        launched = result == ESP_GMF_ERR_OK;
        esp_gmf_pipeline_handle_t pipeline = nullptr;
        tracker.run_attempted(esp_audio_simple_player_get_pipeline(self->player_, &pipeline) == ESP_GMF_ERR_OK &&
                              pipeline != nullptr);
      }
    } else if (command.generation == self->generation_.load()) {
      if (command.operation == Operation::RESUME) self->launch_ms_.store(millis());
      self->pcm_enabled_.store(command.operation == Operation::RESUME);
      result = command.operation == Operation::PAUSE ? esp_audio_simple_player_pause(self->player_)
                                                    : esp_audio_simple_player_resume(self->player_);
    }
    if (result != ESP_GMF_ERR_OK) {
      ESP_LOGE(TAG, "GMF control failed: op=%d result=%d", int(command.operation), int(result));
      {
        std::lock_guard<std::mutex> lock(self->request_mutex_);
        if (command.generation == self->generation_.load()) {
          self->accept_.store(false);
          self->pcm_enabled_.store(false);
          self->transport_.set_epoch(self->epoch_.fetch_add(1) + 1);
          self->failed_.store(true);
        }
      }
      if ((command.operation == Operation::PAUSE || command.operation == Operation::RESUME) && tracker.needs_stop()) {
        const auto stopped = self->stop_(launched);
        tracker.stop_result(stopped == ESP_GMF_ERR_OK);
        launched = false;
        if (stopped != ESP_GMF_ERR_OK) ESP_LOGE(TAG, "Cleanup STOP failed: %d", int(stopped));
      }
    }
    ESP_LOGI(TAG, "Control: generation=%u op=%d elapsed_ms=%u result=%d",
             static_cast<unsigned>(command.generation), int(command.operation),
             static_cast<unsigned>(millis() - start), int(result));
    self->completed_.store(command.generation);
  }
}

int GmfMediaSource::event_(esp_asp_event_pkt_t *packet, void *argument) {
  auto *self = static_cast<GmfMediaSource *>(argument);
  if (packet->type == ESP_ASP_EVENT_TYPE_STATE) {
    const auto state = *static_cast<esp_asp_state_t *>(packet->payload);
    if (state == ESP_ASP_STATE_ERROR || state == ESP_ASP_STATE_FINISHED) {
      self->decoder_task_.store(xTaskGetCurrentTaskHandle());
      self->settle_pending_.store(true);
    }
    if (state == ESP_ASP_STATE_ERROR || state == ESP_ASP_STATE_FINISHED || state == ESP_ASP_STATE_STOPPED)
      self->ended_.store(true);
    std::lock_guard<std::mutex> lock(self->request_mutex_);
    if (!self->accept_.load()) return 0;
    if (state == ESP_ASP_STATE_ERROR) {
      ESP_LOGE(TAG, "GMF pipeline error");
      self->failed_.store(true);
    } else if (state == ESP_ASP_STATE_FINISHED) {
      self->finished_.store(true);
    }
  } else if (packet->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO && self->accept_.load()) {
    const auto *info = static_cast<esp_asp_music_info_t *>(packet->payload);
    if (info->bits != 16 || info->channels < 1 || info->channels > 2 || info->sample_rate <= 0 ||
        info->sample_rate > 48000) {
      ESP_LOGE(TAG, "Unsupported PCM format: %d Hz %d channels %d bits",
               info->sample_rate, info->channels, info->bits);
      self->failed_.store(true);
      self->pcm_enabled_.store(false);
      return 0;
    }
    self->sample_rate_.store(info->sample_rate);
    self->channels_.store(info->channels);
    ESP_LOGI(TAG, "PCM format: %d Hz %d channels %d bits", info->sample_rate, info->channels, info->bits);
  }
  return 0;
}

int GmfMediaSource::pcm_(uint8_t *data, int size, void *argument) {
  auto *self = static_cast<GmfMediaSource *>(argument);
  const auto info = audio::AudioStreamInfo(16, self->channels_.load(), self->sample_rate_.load());
  if (size <= 0) {
    ESP_LOGE(TAG, "Invalid PCM block size: %d", size);
    self->failed_.store(true);
    return -1;
  }
  const auto result = drain_pcm(
      data, size, info.frames_to_bytes(1),
      [self, &info](const uint8_t *buffer, size_t length) {
        const auto count = self->write_output(buffer, length, 20, info);
        if (count <= length) self->accepted_bytes_.fetch_add(count);
        return count;
      },
      [self]() { return self->accept_.load() && self->pcm_enabled_.load() && !self->transport_.cancelled(); },
      []() { return millis(); }, []() { vTaskDelay(1); });
  if (result == TransferResult::INVALID || result == TransferResult::STALLED) {
    ESP_LOGE(TAG, "%s", result == TransferResult::STALLED ? "PCM sink stalled for 10 seconds"
                                                       : "Listener returned invalid PCM acceptance");
    self->failed_.store(true);
    return -1;
  }
  if (result == TransferResult::CANCELLED) vTaskDelay(1);
  return size;
}

void GmfMediaSource::notify_audio_played(uint32_t frames, int64_t) {
  if (!this->accept_.load() || !this->pcm_enabled_.load()) return;
  this->played_frames_.fetch_add(frames);
  this->played_bytes_.fetch_add(frames * this->channels_.load() * 2);
}

void GmfMediaSource::phase_(const char *phase) {
  if (strcmp(this->phase_name_, phase) == 0) return;
  this->phase_name_ = phase;
  ESP_LOGI(TAG, "Phase: %s", phase);
  if (this->phase_sensor_) this->phase_sensor_->publish_state(phase);
}

void GmfMediaSource::loop() {
  if (playback_logic::control_expired(this->pending_(), this->quarantined_.load(), millis(), this->command_ms_)) {
    this->quarantined_.store(true);
    this->accept_.store(false);
    this->pcm_enabled_.store(false);
    this->transport_.set_epoch(this->epoch_.fetch_add(1) + 1);
    ESP_LOGE(TAG, "Control timed out after 10 seconds; output quarantined");
    this->phase_("control_timeout");
    this->set_state_(media_source::MediaSourceState::ERROR);
  }
  if (this->quarantined_.load()) {
    if (!this->pending_()) {
      ESP_LOGE(TAG, "Worker returned after control timeout; source remains quarantined");
      this->mark_failed();
    }
    return;
  }
  if (playback_logic::startup_expired(this->get_state() == media_source::MediaSourceState::PLAYING,
                                    this->launched_.load(), this->played_frames_.load() != 0,
                                    this->pending_(), millis(), this->launch_ms_.load())) {
    ESP_LOGE(TAG, "Playback startup timed out: no DAC PCM within 20 seconds");
    this->failed_.store(true);
  }
  if (this->failed_.exchange(false)) {
    this->request_(Operation::STOP);
    this->set_state_(media_source::MediaSourceState::ERROR);
    this->phase_("error");
  } else if (!this->pending_() && this->finished_.exchange(false)) {
    this->set_state_(media_source::MediaSourceState::IDLE);
    this->phase_("idle");
  } else if (this->played_frames_.load() && this->get_state() == media_source::MediaSourceState::PLAYING) {
    this->phase_("playing");
  }
  char title[128];
  if (this->transport_.take_title(title, sizeof(title)) && this->title_sensor_)
    this->title_sensor_->publish_state(title);
  if (millis() - this->report_ms_ >= 1000) {
    this->report_ms_ = millis();
    ESP_LOGI(TAG, "PCM accepted=%u played=%u rate=%u pending=%d http_close=%u,%u",
             static_cast<unsigned>(this->accepted_bytes_.exchange(0)),
             static_cast<unsigned>(this->played_bytes_.exchange(0)),
             static_cast<unsigned>(this->sample_rate_.load()), int(this->pending_()),
             static_cast<unsigned>(this->transport_.cancel_close.load()),
             static_cast<unsigned>(this->transport_.stale_close.load()));
  }
}

void GmfMediaSource::dump_config() {
  ESP_LOGCONFIG(TAG, "GMF MediaSource: AAC/MP3, ICY, cancellable worker control; native speaker output only");
}

}  // namespace esphome::gmf_audio
#endif
