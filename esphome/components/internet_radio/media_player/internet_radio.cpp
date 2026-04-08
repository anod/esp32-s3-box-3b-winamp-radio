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
  // Audio task on Core 0 — priority 5 to preempt lower-priority tasks
  xTaskCreatePinnedToCore(audio_task, "audio", 12288, this, 5, nullptr, 0);

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

  // Non-blocking PA enable (replaces 200ms blocking delay)
  if (this->pa_pending_ && (millis() - this->pa_pending_ms_ >= 200)) {
    this->pa_pending_ = false;
    digitalWrite(this->pa_pin_, HIGH);
  }

  // Debounced NVS volume save
  this->flush_vol_if_dirty_();

  // Publish state changes to HA (reads from stable title buffer)
  const char *title = this->title_bufs_[this->title_read_idx_];
  if (this->play_state_ != this->last_published_state_ ||
      strcmp(title, this->last_published_title_) != 0) {
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
      self->audio_.connecttohost(self->pending_url_);
    }
    self->audio_.loop();
    vTaskDelay(1);
  }
}

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
    // setVolume is safe to call cross-core
    this->audio_.setVolume(this->vol_);
    this->volume = v;
    this->mark_vol_dirty_();
    ESP_LOGD(TAG, "Volume: %d (%.0f%%)", (int)this->vol_, v * 100);
  }

  if (call.get_media_url().has_value()) {
    const std::string &url = *call.get_media_url();
    ESP_LOGI(TAG, "Play URL: %s", url.c_str());
    strlcpy(this->pending_url_, url.c_str(), sizeof(this->pending_url_));
    this->id3_artist_[0] = '\0';
    this->id3_title_[0] = '\0';
    int write_idx = 1 - this->title_read_idx_;
    strlcpy(this->title_bufs_[write_idx], "Connecting...", 128);
    this->title_read_idx_ = write_idx;
    this->play_state_ = PS_PLAYING;
    this->pending_connect_ = true;
    if (this->pa_pin_ >= 0 && !i2s_bridge::I2SBridge::is_active()) {
      this->pa_pending_ms_ = millis();
      this->pa_pending_ = true;
    }
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
            digitalWrite(this->pa_pin_, LOW);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        int v = this->vol_ + 1;
        if (v > 21) v = 21;
        this->vol_ = v;
        this->audio_.setVolume(v);
        this->volume = (float)v / 21.0f;
        this->mark_vol_dirty_();
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        int v = this->vol_ - 1;
        if (v < 0) v = 0;
        this->vol_ = v;
        this->audio_.setVolume(v);
        this->volume = (float)v / 21.0f;
        this->mark_vol_dirty_();
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
  // Copy URL before setting flag — eliminates ordering race with Core 0
  strlcpy(this->pending_url_, stations_[this->current_station_].url,
          sizeof(this->pending_url_));
  this->id3_artist_[0] = '\0';
  this->id3_title_[0] = '\0';
  // Write title to the buffer Core 0 isn't reading
  int write_idx = 1 - this->title_read_idx_;
  strlcpy(this->title_bufs_[write_idx], "Connecting...", 128);
  this->title_read_idx_ = write_idx;
  this->play_state_ = PS_PLAYING;
  this->pending_connect_ = true;
  // Non-blocking PA enable — schedule for 200ms from now
  if (this->pa_pin_ >= 0 && !i2s_bridge::I2SBridge::is_active()) {
    this->pa_pending_ms_ = millis();
    this->pa_pending_ = true;
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

void InternetRadio::set_volume_direct(int vol) {
  if (vol < 0) vol = 0;
  if (vol > 21) vol = 21;
  this->vol_ = vol;
  this->audio_.setVolume(vol);
  this->volume = (float)vol / 21.0f;
  this->is_muted_ = (vol == 0);
  this->mark_vol_dirty_();
  this->publish_state();
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

// ─── Audio callbacks (runs on Core 0) ─────────────────────

void InternetRadio::audio_callback(Audio::msg_t msg) {
  if (!instance_) return;
  auto *self = instance_;

  switch (msg.e) {
    case Audio::evt_info:
      // Only log at VERBOSE level — UART blocking on Core 0 causes dropouts.
      // BitRate/SampleRate are still parsed below without logging.
      ESP_LOGV(TAG, "info: %s", msg.msg);
      if (msg.msg && strncmp(msg.msg, "BitRate:", 8) == 0) {
        long br = atol(msg.msg + 8);
        if (br > 0) {
          self->bitrate_ = (br >= 1000) ? br / 1000 : br;
        }
      }
      if (msg.msg && strncmp(msg.msg, "SampleRate (Hz):", 16) == 0) {
        uint32_t sr = atol(msg.msg + 16);
        if (sr > 0) {
          i2s_bridge::I2SBridge::bridge_sample_rate = sr;
        }
      }
      break;

    case Audio::evt_streamtitle:
      ESP_LOGI(TAG, "title: %s", msg.msg);
      if (msg.msg && msg.msg[0] != '\0') {
        int wi = 1 - self->title_read_idx_;
        strlcpy(self->title_bufs_[wi], msg.msg, 128);
        self->title_read_idx_ = wi;
      }
      break;

    case Audio::evt_name:
      ESP_LOGI(TAG, "station: %s", msg.msg);
      self->play_state_ = PS_PLAYING;
      {
        const char *cur = self->title_bufs_[self->title_read_idx_];
        if (cur[0] == '\0' || strcmp(cur, "Connecting...") == 0) {
          int wi = 1 - self->title_read_idx_;
          strlcpy(self->title_bufs_[wi], msg.msg, 128);
          self->title_read_idx_ = wi;
        }
      }
      break;

    case Audio::evt_id3data:
      ESP_LOGD(TAG, "id3: %s", msg.msg);
      if (msg.msg) {
        if (strncmp(msg.msg, "Title: ", 7) == 0) {
          strlcpy(self->id3_title_, msg.msg + 7, sizeof(self->id3_title_));
          self->update_id3_song_title_();
        } else if (strncmp(msg.msg, "Artist: ", 8) == 0) {
          strlcpy(self->id3_artist_, msg.msg + 8, sizeof(self->id3_artist_));
          self->update_id3_song_title_();
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
