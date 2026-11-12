#pragma once

#include "esphome/core/defines.h"
#ifdef USE_GMF_MEDIA_SOURCE
#include "http_transport.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <functional>
#include <mutex>

namespace esphome::gmf_audio {

class GmfMediaSource final : public Component, public media_source::MediaSource {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  bool can_handle(const std::string &uri) const override;
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  void notify_audio_played(uint32_t frames, int64_t timestamp) override;
  void set_title_sensor(text_sensor::TextSensor *sensor) { this->title_sensor_ = sensor; }
  void set_phase_sensor(text_sensor::TextSensor *sensor) { this->phase_sensor_ = sensor; }
  void set_pcm_suspension_callback(std::function<void(bool)> callback) {
    this->pcm_suspension_callback_ = std::move(callback);
  }

 protected:
  enum class Operation : uint8_t { STOP, RUN, PAUSE, RESUME };
  struct Command {
    Operation operation;
    uint32_t generation;
    uint32_t epoch;
    char uri[300];
  };
  bool request_(Operation operation, const char *uri = nullptr);
  static void control_task_(void *argument);
  static int pcm_(uint8_t *data, int size, void *argument);
  static int event_(esp_asp_event_pkt_t *packet, void *argument);
  bool settle_();
  esp_gmf_err_t stop_(bool launched);
  bool pending_() const { return this->generation_.load() != this->completed_.load(); }
  void phase_(const char *phase);
  esp_asp_handle_t player_{nullptr};
  esp_gmf_io_handle_t http_{nullptr};
  HttpTransport transport_;
  QueueHandle_t commands_{nullptr};
  std::mutex request_mutex_;
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> completed_{0};
  std::atomic<uint32_t> epoch_{0};
  std::atomic<bool> accept_{false};
  std::atomic<bool> pcm_enabled_{false};
  std::atomic<bool> ended_{true};
  std::atomic<bool> settle_pending_{false};
  std::atomic<TaskHandle_t> decoder_task_{nullptr};
  std::atomic<bool> failed_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> quarantined_{false};
  std::atomic<bool> launched_{false};
  std::atomic<uint32_t> launch_ms_{0};
  std::atomic<uint32_t> played_frames_{0};
  std::atomic<uint32_t> accepted_bytes_{0};
  std::atomic<uint32_t> played_bytes_{0};
  std::atomic<uint32_t> sample_rate_{44100};
  std::atomic<uint8_t> channels_{2};
  uint32_t command_ms_{0};
  uint32_t report_ms_{0};
  const char *phase_name_{"idle"};
  text_sensor::TextSensor *title_sensor_{nullptr};
  text_sensor::TextSensor *phase_sensor_{nullptr};
  std::function<void(bool)> pcm_suspension_callback_;
};

}  // namespace esphome::gmf_audio
#endif
