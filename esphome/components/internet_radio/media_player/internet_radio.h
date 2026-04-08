// ============================================================
// internet_radio — ESPHome MediaPlayer wrapping ESP32-audioI2S
// Runs audio.loop() on Core 0 FreeRTOS task for glitch-free
// HTTP MP3 streaming. Exposes play/pause/stop/volume/next/prev
// to Home Assistant via ESPHome native API.
// ============================================================

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/media_player/media_player.h"

#include <Audio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace internet_radio {

// Station preset
struct Station {
  const char *name;
  const char *url;
};

// Playback state (mirrors current firmware)
enum PlayState : int { PS_STOPPED = 0, PS_PLAYING = 1, PS_PAUSED = 2 };

class InternetRadio : public media_player::MediaPlayer, public Component {
 public:
  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void loop() override;

  // MediaPlayer pure virtuals
  media_player::MediaPlayerTraits get_traits() override;
  bool is_muted() const override { return this->is_muted_; }

  // Config setters (called by codegen from __init__.py)
  void set_i2s_bclk_pin(int pin) { this->bclk_pin_ = pin; }
  void set_i2s_lrclk_pin(int pin) { this->lrclk_pin_ = pin; }
  void set_i2s_dout_pin(int pin) { this->dout_pin_ = pin; }
  void set_i2s_mclk_pin(int pin) { this->mclk_pin_ = pin; }
  void set_pa_pin(int pin) { this->pa_pin_ = pin; }
  void set_default_volume(int vol) { this->default_volume_ = vol; }

  // Public accessors for bridge component and template entities
  Audio &get_audio() { return this->audio_; }
  volatile PlayState &get_play_state() { return this->play_state_; }
  int get_current_station() const { return this->current_station_; }
  const char *get_song_title() const { return this->song_title_; }
  const char *get_station_name() const { return stations_[this->current_station_].name; }
  long get_bitrate() const { return this->bitrate_; }

  // Station control (for select entity)
  void set_station(int idx);
  static constexpr int get_num_stations() { return NUM_STATIONS; }
  static const char *get_station_name_at(int idx) { return (idx >= 0 && idx < NUM_STATIONS) ? stations_[idx].name : ""; }

 protected:
  void control(const media_player::MediaPlayerCall &call) override;

  // Audio callback (static, registered with Audio library)
  static void audio_callback(Audio::msg_t msg);

  // FreeRTOS audio task (static, pinned to Core 0)
  static void audio_task(void *param);

  // Singleton for callbacks (Audio lib uses C-style function pointer)
  static InternetRadio *instance_;

  void connect_station_();
  void update_ha_state_();
  void next_station_();
  void prev_station_();

  // Audio engine
  Audio audio_;

  // Pin configuration
  int bclk_pin_{17};
  int lrclk_pin_{45};
  int dout_pin_{15};
  int mclk_pin_{2};
  int pa_pin_{46};
  int default_volume_{15};

  // Station presets (hardcoded for PoC — will move to YAML config)
  static constexpr int NUM_STATIONS = 10;
  static const Station stations_[NUM_STATIONS];

  // Shared state (audio task on Core 0, ESPHome loop on Core 1)
  volatile int current_station_{0};
  volatile int vol_{15};
  volatile PlayState play_state_{PS_STOPPED};
  volatile long bitrate_{0};
  volatile bool is_muted_{false};
  char song_title_[128]{};

  // Cross-core pending flags (set on Core 1, consumed on Core 0)
  volatile bool pending_connect_{false};
  volatile bool pending_pause_{false};
  volatile bool pending_stop_{false};

  // WiFi / stream lifecycle
  bool wifi_connected_{false};
  bool auto_play_pending_{true};  // connect to station once WiFi is up
  volatile bool stream_failed_{false};
  unsigned long last_retry_ms_{0};
  static constexpr unsigned long RETRY_INTERVAL_MS = 5000;

  // State change detection for HA publishing
  PlayState last_published_state_{PS_STOPPED};
  char last_published_title_[128]{};

  // NVS
  ESPPreferenceObject volume_pref_;
  ESPPreferenceObject station_pref_;
};

}  // namespace internet_radio
}  // namespace esphome
