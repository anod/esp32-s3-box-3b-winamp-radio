// ============================================================
// box3_mic — ESPHome microphone on the BOX-3 ES7210 ADC
// A capture task drains the I2S0 RX channel owned by InternetRadio
// into a stream buffer; loop() (Core 1) delivers it to listeners.
// ============================================================

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "../../internet_radio/media_player/internet_radio.h"

#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

namespace esphome {
namespace box3_mic {

class Box3Mic final : public microphone::Microphone, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE - 3; }

  void start() override;
  void stop() override;

  void set_radio(internet_radio::InternetRadio *radio) { this->radio_ = radio; }
  void set_use_right_channel(bool right) { this->use_right_ = right; }

 protected:
  static void capture_task_(void *param);
  void run_capture_();
  void finish_stop_();

  internet_radio::InternetRadio *radio_{nullptr};
  bool use_right_{false};

  StreamBufferHandle_t stream_{nullptr};
  TaskHandle_t task_{nullptr};
  volatile bool task_stop_{false};
  volatile bool task_done_{true};
  volatile uint32_t dropped_bytes_{0};
  bool want_run_{false};

  static constexpr size_t STREAM_BYTES = 16 * 1024;  // 512 ms of 16 kHz mono
  static constexpr size_t FRAMES_PER_READ = 256;     // 16 ms
};

}  // namespace box3_mic
}  // namespace esphome
