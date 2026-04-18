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
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/select/select.h"

#include <Audio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Audio frame counter: incremented by audio_process_i2s on Core 0,
// read by InternetRadio::loop() on Core 1 for underrun detection.
// A simple increment is near-zero-cost in the audio hot path.
extern volatile uint32_t g_audio_frame_count;

namespace esphome {
namespace internet_radio {

// Station preset
struct Station {
  const char *name;
  const char *url;
};

// Playback state (mirrors current firmware)
enum PlayState : int { PS_STOPPED = 0, PS_PLAYING = 1, PS_PAUSED = 2 };

class InternetRadio final : public media_player::MediaPlayer, public Component {
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

  // Optional HA entity references (for direct publish-on-change)
  void set_now_playing_sensor(text_sensor::TextSensor *s) { this->now_playing_sensor_ = s; }
  void set_station_select(select::Select *s) { this->station_select_ = s; }

  // Public accessors for bridge component and template entities
  Audio &get_audio() { return this->audio_; }
  volatile PlayState &get_play_state() { return this->play_state_; }
  int get_current_station() const { return this->current_station_; }
  const char *get_song_title() const { return this->title_bufs_[this->title_read_idx_]; }
  const char *get_station_name() const { return stations_[this->current_station_].name; }
  long get_bitrate() const { return this->bitrate_; }
  int get_vol() const { return this->vol_; }
  static constexpr int get_max_volume() { return 21; }

  // Map UI volume (0–21) to Audio library volume (0–100).
  // The library's cubic dB curve makes steps 1–8 inaudible at 21 steps.
  // We use 100 steps and skip the bottom of the curve for audible low volumes.
  static uint8_t map_volume_(int vol);

  // Station control (for select entity and display)
  void set_station(int idx);
  static constexpr int get_num_stations() { return NUM_STATIONS; }
  static const char *get_station_name_at(int idx) { return (idx >= 0 && idx < NUM_STATIONS) ? stations_[idx].name : ""; }

  // Station list toggle (normal / test) — saves NVS and reboots
  void toggle_station_list();
  bool is_test_list() const { return this->station_list_ != 0; }

  // Volume control from UI (bypasses HA MediaPlayerCall overhead)
  void set_volume_direct(int vol);

  void next_station_();
  void prev_station_();

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
  void mark_vol_dirty_();
  void flush_vol_if_dirty_();
  void update_id3_song_title_();
  void publish_station_select_();

  // Audio engine
  Audio audio_;

  // Pin configuration
  int bclk_pin_{17};
  int lrclk_pin_{45};
  int dout_pin_{15};
  int mclk_pin_{2};
  int pa_pin_{46};
  int default_volume_{15};

  // Station presets — two lists, pointer to active one
  static constexpr int NUM_STATIONS = 10;
  static const Station stations_normal_[NUM_STATIONS];
  static const Station stations_test_[NUM_STATIONS];
  static const Station *stations_;  // points to active list

  // Shared state (audio task on Core 0, ESPHome loop on Core 1)
  volatile int current_station_{0};
  volatile int vol_{15};
  volatile PlayState play_state_{PS_STOPPED};
  volatile long bitrate_{0};
  volatile bool is_muted_{false};

  // ID3 artist/title buffers (written on Core 0, combined into title_bufs_)
  char id3_artist_[64]{};
  char id3_title_[64]{};

  // Double-buffered song title: Core 0 writes to write buf, then flips index.
  // Core 1 reads from read buf. Avoids torn reads without mutex.
  char title_bufs_[2][128]{};
  volatile int title_read_idx_{0};  // index Core 1 reads from

  // Cross-core pending flags (set on Core 1, consumed on Core 0)
  volatile bool pending_connect_{false};
  volatile bool pending_pause_{false};
  volatile bool pending_stop_{false};
  char pending_url_[256]{};  // URL copied before setting pending_connect_

  // Non-blocking PA enable (avoids 200ms delay in main loop)
  unsigned long pa_pending_ms_{0};
  bool pa_pending_{false};

  // WiFi / stream lifecycle
  bool wifi_connected_{false};
  bool auto_play_pending_{true};  // connect to station once WiFi is up
  volatile bool stream_failed_{false};
  unsigned long last_retry_ms_{0};
  static constexpr unsigned long RETRY_INTERVAL_MS = 5000;

  // Buffer underrun watchdog — reconnects if no audio frames for this long
  static constexpr unsigned long UNDERRUN_TIMEOUT_MS = 10000;
  uint32_t watchdog_last_frame_count_{0};
  unsigned long watchdog_last_check_ms_{0};
  unsigned int watchdog_stall_count_{0};

  // Debounced NVS saves (prevents flash wear from rapid volume changes)
  bool vol_dirty_{false};
  unsigned long vol_dirty_ms_{0};
  static constexpr unsigned long NVS_SAVE_DEBOUNCE_MS = 3000;

  // Station list selection (0=normal, 1=test)
  int station_list_{0};

  // State change detection for HA publishing
  PlayState last_published_state_{PS_STOPPED};
  char last_published_title_[128]{};

  // NVS
  ESPPreferenceObject volume_pref_;
  ESPPreferenceObject station_pref_;
  ESPPreferenceObject list_pref_;

  // Optional HA entity references
  text_sensor::TextSensor *now_playing_sensor_{nullptr};
  select::Select *station_select_{nullptr};
};

}  // namespace internet_radio
}  // namespace esphome
