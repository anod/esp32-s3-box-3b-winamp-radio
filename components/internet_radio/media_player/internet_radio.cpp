// ============================================================
// internet_radio — ESPHome MediaPlayer implementation
// ============================================================

#include "internet_radio.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "../../i2s_bridge/switch/i2s_bridge.h"
#include "driver/gpio.h"
#include "esp_system.h"

// Audio frame counter — incremented by audio_process_i2s on Core 0 (near-zero cost).
// Read by loop() on Core 1 for underrun detection.
// Defined at global scope to match extern declarations in other TUs.
volatile uint32_t g_audio_frame_count = 0;

namespace esphome {
namespace internet_radio {

static const char *const TAG = "internet_radio";

// Volume mapping: UI step (0–21) → Audio library volume (0–100).
// Library uses setVolumeSteps(100) with cubic dB curve. We skip the
// inaudible sub-35 range so step 1 is ~-44 dB instead of ~-60 dB.
static const uint8_t VOL_LUT[22] = {
    0,                                            // 0: mute
    35, 38, 42, 45, 48, 51, 55, 58, 61, 64, 68,  // 1–11: low–mid
    71, 74, 77, 81, 84, 87, 90, 94, 97, 100       // 12–21: mid–full
};

uint8_t InternetRadio::map_volume_(int vol) {
  if (vol <= 0) return 0;
  if (vol >= 21) return 100;
  return VOL_LUT[vol];
}

// Singleton instance for C-style Audio callback
InternetRadio *InternetRadio::instance_ = nullptr;

// Station presets — normal list (matches original firmware)
const Station InternetRadio::stations_normal_[NUM_STATIONS] = {
    {"Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"Enigmatic Station 1", "http://listen2.myradio24.com/8226"},
    {"Psytrance", "http://hirschmilch.de:7000/psytrance.mp3"},
    {"Rock Antenne", "http://stream.rockantenne.de/rockantenne/stream/mp3"},
    {"181 Hard Rock", "http://listen.181fm.com/181-hardrock_128k.mp3"},
    {"Heavy Metal", "http://stream.rockantenne.de/heavy-metal/stream/mp3"},
    {"181 Power Hits", "http://listen.181fm.com/181-power_128k.mp3"},
    {"KROQ", "https://live.amperwave.net/direct/audacy-kroqfmaac-imc"},
    {"BBC World News", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
    {"NPR News", "http://npr-ice.streamguys1.com/live.mp3"},
};

// Station presets — stress test list (high bitrate / different codecs)
const Station InternetRadio::stations_test_[NUM_STATIONS] = {
    {"MP3 320k Jazz",    "http://mediaserv38.live-streams.nl:8006/live"},
    {"MP3 320k Pop",     "http://mediaserv30.live-streams.nl:8086/live"},
    {"MP3 320k Classic", "http://mediaserv30.live-streams.nl:8088/live"},
    {"AAC 320k Naim",    "http://mscp3.live-streams.nl:8360/high.aac"},
    {"AAC 320k Jazz",    "http://mscp3.live-streams.nl:8340/jazz-high.aac"},
    {"FLAC 96k Lossless", "https://stream.motherearthradio.de/listen/motherearth/motherearth.flac-lo"},
    {"TLS 320k Blues",   "https://blueswave.radio:8002/blues320"},
    {"MP3 320k Gold",    "http://mediaserv30.live-streams.nl:8000/live"},
    {"MP3 320k Lounge",  "http://mediaserv33.live-streams.nl:8036/live"},
    {"MP3 320k World",   "http://mediaserv38.live-streams.nl:8027/live"},
};

// Active list pointer (set in setup based on NVS)
const Station *InternetRadio::stations_ = InternetRadio::stations_normal_;

// ─── Setup ────────────────────────────────────────────────

void InternetRadio::setup() {
  instance_ = this;

  // Restore saved state
  this->volume_pref_ = global_preferences->make_preference<int>(fnv1_hash("radio_vol"));
  this->station_pref_ = global_preferences->make_preference<int>(fnv1_hash("radio_sta"));
  this->list_pref_ = global_preferences->make_preference<int>(fnv1_hash("radio_list"));

  int saved_vol = this->default_volume_;
  int saved_sta = 0;
  int saved_list = 0;
  this->volume_pref_.load(&saved_vol);
  this->station_pref_.load(&saved_sta);
  this->list_pref_.load(&saved_list);

  if (saved_vol < 0 || saved_vol > 21) saved_vol = this->default_volume_;
  if (saved_sta < 0 || saved_sta >= NUM_STATIONS) saved_sta = 0;
  if (saved_list < 0 || saved_list > 1) saved_list = 0;
  this->vol_ = saved_vol;
  this->current_station_ = saved_sta;
  this->station_list_ = saved_list;

  // Set active station list pointer
  stations_ = saved_list ? stations_test_ : stations_normal_;

  // Override HA select options to match active list
  if (this->station_select_) {
    FixedVector<const char *> opts;
    opts.init(NUM_STATIONS);
    for (int i = 0; i < NUM_STATIONS; i++)
      opts.push_back(stations_[i].name);
    this->station_select_->traits.set_options(opts);
  }

  ESP_LOGI(TAG, "Restored station=%d vol=%d list=%s",
           (int)this->current_station_, (int)this->vol_,
           saved_list ? "TEST" : "NORMAL");

  // PA pin — keep LOW until stream connects
  if (this->pa_pin_ >= 0) {
    auto gpio = static_cast<gpio_num_t>(this->pa_pin_);
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, 0);
  }

  // Configure Audio library — STUB: no audio backend yet (ESP-ADF in Step 2)
  // Audio::audio_info_callback = audio_callback;
  // this->audio_.setPinout(...);
  // this->audio_.setVolumeSteps(100);
  // this->audio_.setVolume(map_volume_(this->vol_));
  // this->audio_.setConnectionTimeout(5000, 0);

  // Set initial HA state
  this->volume = (float)this->vol_ / 21.0f;
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;

  ESP_LOGI(TAG, "Audio backend: STUB (no playback — ESP-ADF pending)");

  // Audio task on Core 0 — STUB: not launched until ESP-ADF is integrated
  // xTaskCreatePinnedToCore(audio_task, "audio", 12288, this, 5, nullptr, 0);
}

// ─── Loop (Core 1 — ESPHome main loop) ───────────────────

void InternetRadio::loop() {
  bool wifi_now = network::is_connected();

  // Detect WiFi connect
  if (wifi_now && !this->wifi_connected_) {
    this->wifi_connected_ = true;
    ESP_LOGI(TAG, "Network connected, starting stream");
    if (this->auto_play_pending_) {
      this->auto_play_pending_ = false;
      this->connect_station_();
      this->publish_station_select_();
    }
  }
  // Detect WiFi disconnect
  if (!wifi_now && this->wifi_connected_) {
    this->wifi_connected_ = false;
    ESP_LOGW(TAG, "WiFi lost");
  }

  // Retry on stream failure
  if (this->stream_failed_ && this->wifi_connected_) {
    unsigned long now = millis();
    if (now - this->last_retry_ms_ >= RETRY_INTERVAL_MS) {
      ESP_LOGI(TAG, "Retrying stream...");
      this->stream_failed_ = false;
      this->connect_station_();
    }
  }

  // Buffer underrun watchdog: if playing but no audio frames produced for
  // UNDERRUN_TIMEOUT_MS, the stream is stalled (ring buffer drained, library
  // stuck in error loop). Reconnect to recover — the Audio library won't
  // fire evt_eof in this state.
  // Uses a frame counter (incremented on Core 0) instead of millis() in the
  // audio callback to avoid any overhead in the hot audio path.
  {
    unsigned long now = millis();
    if (now - this->watchdog_last_check_ms_ >= 1000) {
      this->watchdog_last_check_ms_ = now;
      uint32_t fc = g_audio_frame_count;
      if (this->play_state_ == PS_PLAYING && this->wifi_connected_ &&
          !this->stream_failed_ && !this->pending_connect_) {
        if (fc == this->watchdog_last_frame_count_ &&
            this->watchdog_stall_count_++ >= (UNDERRUN_TIMEOUT_MS / 1000)) {
          ESP_LOGW(TAG, "No audio output for %lus, reconnecting",
                   (unsigned long)(UNDERRUN_TIMEOUT_MS / 1000));
          this->watchdog_stall_count_ = 0;
          this->connect_station_();
        }
      } else {
        this->watchdog_stall_count_ = 0;
      }
      this->watchdog_last_frame_count_ = fc;
    }
  }

  // Non-blocking PA enable (replaces 200ms blocking delay)
  if (this->pa_pending_ && (millis() - this->pa_pending_ms_ >= 200)) {
    this->pa_pending_ = false;
    gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 1);
  }

  // Debounced NVS volume save
  this->flush_vol_if_dirty_();

  // Publish state changes to HA (reads from stable title buffer)
  const char *title = this->title_bufs_[this->title_read_idx_];
  bool title_changed = strcmp(title, this->last_published_title_) != 0;
  if (this->play_state_ != this->last_published_state_ || title_changed) {
    this->update_ha_state_();
  }
  if (title_changed && this->now_playing_sensor_) {
    this->now_playing_sensor_->publish_state(title);
  }
}

// ─── Audio task (Core 0) — STUB ───────────────────────────
// Will be replaced by ESP-ADF pipeline tasks in Step 2.
// No audio_task launched — pipeline manages its own tasks.

// ─── MediaPlayer interface ────────────────────────────────

media_player::MediaPlayerTraits InternetRadio::get_traits() {
  media_player::MediaPlayerTraits traits;
  traits.set_supports_pause(true);
  // NEXT_TRACK/PREVIOUS_TRACK: the ESPHome API protobuf MediaPlayerCommand
  // enum stops at TURN_OFF=13 — values 14/15 are NOT transmitted. aioesphomeapi
  // and the HA ESPHome integration also lack these. Even speaker_source has the
  // same gap (confirmed March 2026). Use button entities for next/prev.
  traits.add_feature_flags(
      media_player::MediaPlayerEntityFeature::PLAY |
      media_player::MediaPlayerEntityFeature::STOP |
      media_player::MediaPlayerEntityFeature::VOLUME_SET |
      media_player::MediaPlayerEntityFeature::VOLUME_MUTE |
      media_player::MediaPlayerEntityFeature::VOLUME_STEP |
      media_player::MediaPlayerEntityFeature::PLAY_MEDIA |
      media_player::MediaPlayerEntityFeature::BROWSE_MEDIA |
      media_player::MediaPlayerEntityFeature::TURN_ON |
      media_player::MediaPlayerEntityFeature::TURN_OFF);
  return traits;
}

void InternetRadio::control(const media_player::MediaPlayerCall &call) {
  if (call.get_volume().has_value()) {
    float v = *call.get_volume();
    this->vol_ = (int)(v * 21.0f + 0.5f);
    if (this->vol_ < 0) this->vol_ = 0;
    if (this->vol_ > 21) this->vol_ = 21;
    // STUB: no audio backend — volume will be set via ES8311 I2C in Step 2
    this->volume = v;
    this->mark_vol_dirty_();
    ESP_LOGD(TAG, "Volume: %d (%.0f%%)", (int)this->vol_, v * 100);
  }

  if (call.get_media_url().has_value()) {
    const std::string &url = *call.get_media_url();
    ESP_LOGI(TAG, "Play URL: %s (STUB: no audio)", url.c_str());
    strlcpy(this->pending_url_, url.c_str(), sizeof(this->pending_url_));
    this->id3_artist_[0] = '\0';
    this->id3_title_[0] = '\0';
    int write_idx = 1 - this->title_read_idx_;
    strlcpy(this->title_bufs_[write_idx], "No audio backend", 128);
    this->title_read_idx_ = write_idx;
    // STUB: stay IDLE
    this->play_state_ = PS_STOPPED;
    this->watchdog_stall_count_ = 0;
  }

  if (call.get_command().has_value()) {
    ESP_LOGI(TAG, "Command received: %d", (int)*call.get_command());
    switch (*call.get_command()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        if (this->play_state_ == PS_PAUSED) {
          this->pending_pause_ = true;  // toggles pause
        } else if (this->play_state_ == PS_STOPPED) {
          this->connect_station_();
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
        if (this->play_state_ == PS_PLAYING) {
          this->pending_pause_ = true;
          this->play_state_ = PS_PAUSED;
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        if (this->play_state_ != PS_STOPPED) {
          this->pending_stop_ = true;
          this->play_state_ = PS_STOPPED;
          if (this->pa_pin_ >= 0 && !i2s_bridge::I2SBridge::is_active())
            gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 0);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (this->play_state_ == PS_PLAYING) {
          this->pending_pause_ = true;
          this->play_state_ = PS_PAUSED;
        } else if (this->play_state_ == PS_PAUSED) {
          this->pending_pause_ = true;
        } else {
          this->connect_station_();
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_TURN_ON:
        if (this->play_state_ == PS_STOPPED) {
          this->connect_station_();
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_TURN_OFF:
        if (this->play_state_ != PS_STOPPED) {
          this->pending_stop_ = true;
          this->play_state_ = PS_STOPPED;
          if (this->pa_pin_ >= 0 && !i2s_bridge::I2SBridge::is_active())
            gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 0);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        int v = this->vol_ + 1;
        if (v > 21) v = 21;
        this->vol_ = v;
        // STUB: no audio backend
        this->volume = (float)v / 21.0f;
        this->mark_vol_dirty_();
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        int v = this->vol_ - 1;
        if (v < 0) v = 0;
        this->vol_ = v;
        // STUB: no audio backend
        this->volume = (float)v / 21.0f;
        this->mark_vol_dirty_();
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        if (!this->is_muted_) {
          this->is_muted_ = true;
          // STUB: no audio backend
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        if (this->is_muted_) {
          this->is_muted_ = false;
          // STUB: no audio backend
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_NEXT:
        this->next_station_();
        break;

      case media_player::MEDIA_PLAYER_COMMAND_PREVIOUS:
        this->prev_station_();
        break;

      default:
        break;
    }
  }

  this->update_ha_state_();
}

// ─── Internal helpers ─────────────────────────────────────

void InternetRadio::connect_station_() {
  ESP_LOGI(TAG, "Station: [%d] %s → %s (STUB: no audio)",
           (int)this->current_station_,
           stations_[this->current_station_].name,
           stations_[this->current_station_].url);
  // Copy URL for future use
  strlcpy(this->pending_url_, stations_[this->current_station_].url,
          sizeof(this->pending_url_));
  this->id3_artist_[0] = '\0';
  this->id3_title_[0] = '\0';
  // Write station name as title (no stream = no ICY metadata)
  int write_idx = 1 - this->title_read_idx_;
  strlcpy(this->title_bufs_[write_idx], stations_[this->current_station_].name, 128);
  this->title_read_idx_ = write_idx;
  // STUB: stay IDLE since no audio backend
  this->play_state_ = PS_STOPPED;
  this->watchdog_stall_count_ = 0;
}

void InternetRadio::next_station_() {
  int sta = (this->current_station_ + 1) % NUM_STATIONS;
  this->current_station_ = sta;
  int save_sta = sta;
  this->station_pref_.save(&save_sta);
  this->connect_station_();
  this->publish_station_select_();
}

void InternetRadio::prev_station_() {
  int sta = (this->current_station_ - 1 + NUM_STATIONS) % NUM_STATIONS;
  this->current_station_ = sta;
  int save_sta = sta;
  this->station_pref_.save(&save_sta);
  this->connect_station_();
  this->publish_station_select_();
}

void InternetRadio::set_station(int idx) {
  if (idx < 0 || idx >= NUM_STATIONS || idx == this->current_station_) return;
  this->current_station_ = idx;
  int save_sta = idx;
  this->station_pref_.save(&save_sta);
  this->connect_station_();
  this->publish_station_select_();
}

void InternetRadio::set_volume_direct(int vol) {
  if (vol < 0) vol = 0;
  if (vol > 21) vol = 21;
  this->vol_ = vol;
  // STUB: no audio backend — volume will be set via ES8311 I2C in Step 2
  this->volume = (float)vol / 21.0f;
  this->is_muted_ = (vol == 0);
  this->mark_vol_dirty_();
  this->publish_state();
}

void InternetRadio::toggle_station_list() {
  this->station_list_ = 1 - this->station_list_;
  int save_list = this->station_list_;
  this->list_pref_.save(&save_list);
  // Reset to station 0 — indices don't map across lists
  int zero = 0;
  this->station_pref_.save(&zero);
  // Flush NVS before reboot — save() only queues, sync() writes to flash
  global_preferences->sync();
  ESP_LOGI(TAG, "Switching to %s list, rebooting...",
           this->station_list_ ? "TEST" : "NORMAL");
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
}

void InternetRadio::update_ha_state_() {
  switch (this->play_state_) {
    case PS_PLAYING:
      this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
      break;
    case PS_PAUSED:
      this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
      break;
    case PS_STOPPED:
    default:
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      break;
  }
  this->volume = (float)this->vol_ / 21.0f;
  this->publish_state();
  this->last_published_state_ = this->play_state_;
  const char *title = this->title_bufs_[this->title_read_idx_];
  strlcpy(this->last_published_title_, title, sizeof(this->last_published_title_));
}

void InternetRadio::mark_vol_dirty_() {
  this->vol_dirty_ = true;
  this->vol_dirty_ms_ = millis();
}

void InternetRadio::flush_vol_if_dirty_() {
  if (this->vol_dirty_ && (millis() - this->vol_dirty_ms_ >= NVS_SAVE_DEBOUNCE_MS)) {
    this->vol_dirty_ = false;
    int save_vol = this->vol_;
    this->volume_pref_.save(&save_vol);
    ESP_LOGD(TAG, "NVS: saved volume=%d", save_vol);
  }
}

void InternetRadio::publish_station_select_() {
  if (this->station_select_) {
    int idx = this->current_station_;
    if (idx >= 0 && idx < NUM_STATIONS)
      this->station_select_->publish_state(stations_[idx].name);
  }
}

// Combine id3_artist_ and id3_title_ into "Artist - Title" display string.
// Called on Core 0 from audio callback.
void InternetRadio::update_id3_song_title_() {
  char combined[128];
  if (this->id3_title_[0] && this->id3_artist_[0]) {
    snprintf(combined, sizeof(combined), "%s - %s", this->id3_artist_, this->id3_title_);
  } else if (this->id3_title_[0]) {
    strlcpy(combined, this->id3_title_, sizeof(combined));
  } else if (this->id3_artist_[0]) {
    strlcpy(combined, this->id3_artist_, sizeof(combined));
  } else {
    return;
  }
  int wi = 1 - this->title_read_idx_;
  if (strcmp(this->title_bufs_[wi], combined) == 0) return;
  strlcpy(this->title_bufs_[wi], combined, 128);
  this->title_read_idx_ = wi;
}

// ─── Audio callbacks — STUB ───────────────────────────────
// Will be replaced by ESP-ADF event handling in Step 2.

}  // namespace internet_radio
}  // namespace esphome
