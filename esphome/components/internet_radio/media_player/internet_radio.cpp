// ============================================================
// internet_radio — ESPHome MediaPlayer implementation
// ============================================================

#include "internet_radio.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "../../i2s_bridge/switch/i2s_bridge.h"

namespace esphome {
namespace internet_radio {

static const char *const TAG = "internet_radio";

// Singleton instance for C-style Audio callback
InternetRadio *InternetRadio::instance_ = nullptr;

// Station presets (PoC — matches current firmware)
const Station InternetRadio::stations_[NUM_STATIONS] = {
    {"Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"Enigmatic Station 1", "http://listen2.myradio24.com/8226"},
    {"Psytrance", "http://hirschmilch.de:7000/psytrance.mp3"},
    {"Rock Antenne", "http://stream.rockantenne.de/rockantenne/stream/mp3"},
    {"181 Hard Rock", "http://listen.181fm.com/181-hardrock_128k.mp3"},
    {"Heavy Metal", "http://stream.rockantenne.de/heavy-metal/stream/mp3"},
    {"181 Power Hits", "http://listen.181fm.com/181-power_128k.mp3"},
    {"KROQ", "https://live.amperwave.net/manifest/audacy-kroqfmaac-hlsc.m3u8"},
    {"BBC World News", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
    {"NPR News", "http://npr-ice.streamguys1.com/live.mp3"},
};

// ─── Setup ────────────────────────────────────────────────

void InternetRadio::setup() {
  instance_ = this;

  // Restore saved state
  this->volume_pref_ = global_preferences->make_preference<int>(fnv1_hash("radio_vol"));
  this->station_pref_ = global_preferences->make_preference<int>(fnv1_hash("radio_sta"));

  int saved_vol = this->default_volume_;
  int saved_sta = 0;
  this->volume_pref_.load(&saved_vol);
  this->station_pref_.load(&saved_sta);

  if (saved_vol < 0 || saved_vol > 21) saved_vol = this->default_volume_;
  if (saved_sta < 0 || saved_sta >= NUM_STATIONS) saved_sta = 0;
  this->vol_ = saved_vol;
  this->current_station_ = saved_sta;

  ESP_LOGI(TAG, "Restored station=%d vol=%d", (int)this->current_station_, (int)this->vol_);

  // PA pin — keep LOW until stream connects
  if (this->pa_pin_ >= 0) {
    pinMode(this->pa_pin_, OUTPUT);
    digitalWrite(this->pa_pin_, LOW);
  }

  // Configure Audio library
  Audio::audio_info_callback = audio_callback;
  this->audio_.setPinout(this->bclk_pin_, this->lrclk_pin_, this->dout_pin_, this->mclk_pin_);
  this->audio_.setVolume(this->vol_);
  this->audio_.setConnectionTimeout(5000, 0);

  // Set initial HA state
  this->volume = (float)this->vol_ / 21.0f;
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;

  // Don't connect yet — wait for WiFi in loop()
  ESP_LOGI(TAG, "Waiting for WiFi before streaming...");

  // Launch audio task on Core 0
  xTaskCreatePinnedToCore(audio_task, "audio", 12288, this, 2, nullptr, 0);

  ESP_LOGI(TAG, "Audio task started on Core 0");
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

  // Publish state changes to HA
  if (this->play_state_ != this->last_published_state_ ||
      strcmp(this->song_title_, this->last_published_title_) != 0) {
    this->update_ha_state_();
  }
}

// ─── Audio task (Core 0) ──────────────────────────────────

void InternetRadio::audio_task(void *param) {
  auto *self = static_cast<InternetRadio *>(param);
  for (;;) {
    if (self->pending_stop_) {
      self->pending_stop_ = false;
      self->audio_.stopSong();
    }
    if (self->pending_pause_) {
      self->pending_pause_ = false;
      self->audio_.pauseResume();
    }
    if (self->pending_connect_) {
      self->pending_connect_ = false;
      self->audio_.connecttohost(self->stations_[self->current_station_].url);
    }
    self->audio_.loop();
    vTaskDelay(1);
  }
}

// ─── MediaPlayer interface ────────────────────────────────

media_player::MediaPlayerTraits InternetRadio::get_traits() {
  media_player::MediaPlayerTraits traits;
  traits.set_supports_pause(true);
  traits.add_feature_flags(
      media_player::MediaPlayerEntityFeature::PLAY |
      media_player::MediaPlayerEntityFeature::STOP |
      media_player::MediaPlayerEntityFeature::VOLUME_SET |
      media_player::MediaPlayerEntityFeature::VOLUME_MUTE |
      media_player::MediaPlayerEntityFeature::VOLUME_STEP |
      media_player::MediaPlayerEntityFeature::PLAY_MEDIA);
  return traits;
}

void InternetRadio::control(const media_player::MediaPlayerCall &call) {
  if (call.get_volume().has_value()) {
    float v = *call.get_volume();
    this->vol_ = (int)(v * 21.0f + 0.5f);
    if (this->vol_ < 0) this->vol_ = 0;
    if (this->vol_ > 21) this->vol_ = 21;
    // setVolume is safe to call cross-core
    this->audio_.setVolume(this->vol_);
    this->volume = v;
    // Save to NVS
    int save_vol = this->vol_;
    this->volume_pref_.save(&save_vol);
    ESP_LOGD(TAG, "Volume: %d (%.0f%%)", (int)this->vol_, v * 100);
  }

  if (call.get_media_url().has_value()) {
    // Play a specific URL (from HA media browser or TTS)
    const std::string &url = *call.get_media_url();
    ESP_LOGI(TAG, "Play URL: %s", url.c_str());
    // For now, use connecttohost directly — URL is temporary so we need to
    // copy it and let the audio task pick it up. For PoC, set pending connect
    // with the station URL mechanism.
    // TODO: support arbitrary URLs beyond preset stations
    this->pending_connect_ = true;
  }

  if (call.get_command().has_value()) {
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
            digitalWrite(this->pa_pin_, LOW);
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

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        int v = this->vol_ + 1;
        if (v > 21) v = 21;
        this->vol_ = v;
        this->audio_.setVolume(v);
        this->volume = (float)v / 21.0f;
        int save_vol = v;
        this->volume_pref_.save(&save_vol);
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        int v = this->vol_ - 1;
        if (v < 0) v = 0;
        this->vol_ = v;
        this->audio_.setVolume(v);
        this->volume = (float)v / 21.0f;
        int save_vol = v;
        this->volume_pref_.save(&save_vol);
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        if (!this->is_muted_) {
          this->is_muted_ = true;
          this->audio_.setVolume(0);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        if (this->is_muted_) {
          this->is_muted_ = false;
          this->audio_.setVolume(this->vol_);
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
  ESP_LOGI(TAG, "Connecting: [%d] %s → %s",
           (int)this->current_station_,
           stations_[this->current_station_].name,
           stations_[this->current_station_].url);
  strlcpy(this->song_title_, "Connecting...", sizeof(this->song_title_));
  this->play_state_ = PS_PLAYING;
  this->pending_connect_ = true;
  // Enable PA only when NOT routing to BT bridge
  if (this->pa_pin_ >= 0 && !i2s_bridge::I2SBridge::is_active()) {
    delay(200);
    digitalWrite(this->pa_pin_, HIGH);
  }
}

void InternetRadio::next_station_() {
  int sta = (this->current_station_ + 1) % NUM_STATIONS;
  this->current_station_ = sta;
  int save_sta = sta;
  this->station_pref_.save(&save_sta);
  this->connect_station_();
}

void InternetRadio::prev_station_() {
  int sta = (this->current_station_ - 1 + NUM_STATIONS) % NUM_STATIONS;
  this->current_station_ = sta;
  int save_sta = sta;
  this->station_pref_.save(&save_sta);
  this->connect_station_();
}

void InternetRadio::set_station(int idx) {
  if (idx < 0 || idx >= NUM_STATIONS || idx == this->current_station_) return;
  this->current_station_ = idx;
  int save_sta = idx;
  this->station_pref_.save(&save_sta);
  this->connect_station_();
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
  strlcpy(this->last_published_title_, this->song_title_, sizeof(this->last_published_title_));
}

// ─── Audio callbacks (runs on Core 0) ─────────────────────

void InternetRadio::audio_callback(Audio::msg_t msg) {
  if (!instance_) return;
  auto *self = instance_;

  switch (msg.e) {
    case Audio::evt_info:
      ESP_LOGD(TAG, "info: %s", msg.msg);
      if (msg.msg && strncmp(msg.msg, "BitRate:", 8) == 0) {
        long br = atol(msg.msg + 8);
        if (br > 0) {
          self->bitrate_ = (br >= 1000) ? br / 1000 : br;
        }
      }
      break;

    case Audio::evt_streamtitle:
      ESP_LOGI(TAG, "title: %s", msg.msg);
      if (msg.msg && msg.msg[0] != '\0') {
        strlcpy(self->song_title_, msg.msg, sizeof(self->song_title_));
      }
      break;

    case Audio::evt_name:
      ESP_LOGI(TAG, "station: %s", msg.msg);
      self->play_state_ = PS_PLAYING;
      if (self->song_title_[0] == '\0' ||
          strcmp(self->song_title_, "Connecting...") == 0) {
        strlcpy(self->song_title_, msg.msg, sizeof(self->song_title_));
      }
      break;

    case Audio::evt_id3data:
      ESP_LOGD(TAG, "id3: %s", msg.msg);
      if (msg.msg) {
        if (strncmp(msg.msg, "Title: ", 7) == 0) {
          strlcpy(self->song_title_, msg.msg + 7, sizeof(self->song_title_));
        }
      }
      break;

    case Audio::evt_bitrate:
      if (msg.msg) {
        long br = atol(msg.msg);
        if (br > 0) {
          self->bitrate_ = (br >= 1000) ? br / 1000 : br;
        }
      }
      break;

    case Audio::evt_eof:
      ESP_LOGW(TAG, "Stream ended");
      self->play_state_ = PS_STOPPED;
      self->stream_failed_ = true;
      self->last_retry_ms_ = millis();
      break;

    case Audio::evt_log:
      ESP_LOGD(TAG, "log: %s", msg.msg);
      if (msg.msg && (strstr(msg.msg, "failed") || strstr(msg.msg, "error"))) {
        ESP_LOGW(TAG, "Stream error detected, will retry");
        self->stream_failed_ = true;
        self->last_retry_ms_ = millis();
      }
      break;

    default:
      break;
  }
}

}  // namespace internet_radio
}  // namespace esphome
