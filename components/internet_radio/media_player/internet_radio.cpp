// ============================================================
// internet_radio — ESPHome MediaPlayer implementation
// ESP-GMF pipeline: HTTP → decoder → PCM callback → I2S0
// ============================================================

#include "internet_radio.h"
#include "voice_logic.h"
#include "playback_logic.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "../../i2s_bridge/switch/i2s_bridge.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>

// FFT sample feed (defined in spectrum.cpp)
extern void feed_fft_samples(const uint8_t *data, int size);

// Audio frame counter — incremented in pcm_output_cb_ on Core 0.
// Read by loop() on Core 1 for underrun detection.
volatile uint32_t g_audio_frame_count = 0;

namespace esphome::gmf_audio {
void initialize_codec_memory();
}

namespace esphome {
namespace internet_radio {

static const char *const TAG = "internet_radio";
static std::atomic<size_t> failed_alloc_size{0};
static std::atomic<uint32_t> failed_alloc_caps{0};
static std::atomic<size_t> failed_alloc_internal{0};
static std::atomic<size_t> failed_alloc_largest{0};

static void allocation_failed(size_t size, uint32_t caps, const char *) {
  failed_alloc_caps.store(caps);
  failed_alloc_internal.store(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  failed_alloc_largest.store(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  failed_alloc_size.store(size);
}

// Software volume: UI step (0–21) → Q8 fixed-point gain.
// Perceptual dB curve capped at unity; hot speech streams can clip if the
// software path boosts above 0 dB before the ES8311 analog stage.
static const uint16_t VOL_Q8[22] = {
    0,                                          // 0: mute
    3,   3,   5,   6,   8,  10,  13,  17,      // 1–8:  ~-40 to -24 dB
    22,  28,  37,  48,  62,  81, 106, 138,      // 9–16: ~-21 to -5 dB
    179, 231, 248, 252, 256                     // 17–21: approach unity
};

// Voice assistant replies use a fixed medium level (step 14 ≈ -10 dB),
// independent of the radio volume.
static constexpr int VOICE_REPLY_GAIN_Q8 = 81;

int InternetRadio::map_vol_q8_(int step) {
  if (step <= 0) return 0;
  if (step >= 21) return 256;
  return VOL_Q8[step];
}

// ES8311 hardware DAC: +6 dB boost for internal speaker analog stage.

// Singleton instance for C-style Audio callback
InternetRadio *InternetRadio::instance_ = nullptr;

// Station presets — normal list (matches original firmware)
const Station InternetRadio::stations_normal_[NUM_STATIONS] = {
    {"Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"Enigmatic Station 1", "http://listen2.myradio24.com/8226"},
    {"Psytrance", "http://hirschmilch.de:7000/psytrance.mp3"},
    {"Rock Antenne", "http://stream.rockantenne.de/rockantenne/stream/mp3"},
    {"181 Hard Rock", "http://listen.181fm.com/181-hardrock_128k.mp3"},
    {"KEXP 90.3 Seattle", "https://kexp-mp3-128.streamguys1.com/kexp128.mp3"},
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
  heap_caps_register_failed_alloc_callback(allocation_failed);

  // Restore saved state
  this->volume_pref_ = global_preferences->make_preference<int>(fnv1_hash("radio_vol"));
  this->station_pref_ = global_preferences->make_preference<int>(fnv1_hash("radio_sta"));
  this->list_pref_ = global_preferences->make_preference<int>(fnv1_hash("radio_list"));
  this->play_pref_ = global_preferences->make_preference<int>(fnv1_hash("radio_play"));

  int saved_vol = this->default_volume_;
  int saved_sta = 0;
  int saved_list = 0;
  int saved_play = 0;
  this->volume_pref_.load(&saved_vol);
  this->station_pref_.load(&saved_sta);
  this->list_pref_.load(&saved_list);
  this->play_pref_.load(&saved_play);

  if (saved_vol < 0 || saved_vol > 21) saved_vol = this->default_volume_;
  if (saved_sta < 0 || saved_sta >= NUM_STATIONS) saved_sta = 0;
  if (saved_list < 0 || saved_list > 1) saved_list = 0;
  if (saved_play < 0 || saved_play > 1) saved_play = 0;
  this->vol_ = saved_vol;
  this->current_station_ = saved_sta;
  this->station_list_ = saved_list;
  this->resume_playback_ = saved_play != 0;
  this->auto_play_pending_ = this->resume_playback_;

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

  ESP_LOGI(TAG, "Restored station=%d vol=%d list=%s resume=%s",
           (int)this->current_station_, (int)this->vol_,
           saved_list ? "TEST" : "NORMAL", this->resume_playback_ ? "yes" : "no");

  // PA pin — keep LOW until stream connects
  if (this->pa_pin_ >= 0) {
    auto gpio = static_cast<gpio_num_t>(this->pa_pin_);
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, 0);
  }

  // Set software volume from NVS BEFORE hardware init (codec may fail on cold boot)
  this->sw_vol_gain_ = this->is_muted_ ? 0 : map_vol_q8_(this->vol_);

  // Initialize I2S and player — ES8311 codec deferred to loop() (I2C/MCLK not ready)
  this->init_i2s0_();
  this->init_player_();

  // Set initial HA state
  this->volume = (float)this->vol_ / 21.0f;
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
}

// ─── Loop (Core 1 — ESPHome main loop) ───────────────────

void InternetRadio::loop() {
  const uint32_t loop_start = micros();
  const unsigned long loop_now = millis();
  if (this->diag_loop_last_ms_ != 0)
    this->diag_loop_gap_max_ms_ = std::max(this->diag_loop_gap_max_ms_, uint32_t(loop_now - this->diag_loop_last_ms_));
  this->diag_loop_last_ms_ = loop_now;
  if (playback_logic::startup_expired(this->playback_requested_.load(), this->playback_launched_.load(),
                                    this->pcm_started_.load(), this->player_command_pending_(), loop_now,
                                    this->playback_start_ms_.load())) {
    const bool announcement = this->announcing_;
    ESP_LOGW(TAG, "Playback startup timed out: no PCM within 20s");
    if (this->request_player_(PlayerOperation::STOP)) {
      this->announcing_ = false;
      this->announce_pending_ = false;
      this->stream_failed_ = !announcement && !this->voice_active_;
    }
  }
  if (playback_logic::control_expired(this->player_command_pending_(), this->player_control_fault_.load(),
                                    millis(), this->player_command_ms_)) {
    this->player_control_fault_.store(true);
    this->accept_player_events_.store(false);
    this->pcm_enabled_.store(false);
    ESP_LOGE(TAG, "Player control timed out after 10s; audio quarantined until worker returns");
  }
  if (this->player_control_fault_.load() && !this->player_command_pending_()) {
    this->player_control_fault_.store(false);
    this->announcing_ = false;
    this->announce_pending_ = false;
    this->play_state_ = PS_STOPPED;
    this->player_running_ = false;
    this->playback_requested_.store(false);
    this->stream_failed_ = !this->voice_active_ && this->resume_playback_;
    ESP_LOGW(TAG, "Player worker returned after timeout; recovery permitted");
  }
  // Deferred ES8311 init — retry every 2 seconds until success
  if (!this->codec_if_) {
    static uint32_t last_codec_try = 0;
    uint32_t now = millis();
    if (now - last_codec_try >= 2000) {
      last_codec_try = now;
      this->init_es8311_();
    }
  }

  bool wifi_now = network::is_connected();

  if (this->voice_cue_done_.exchange(false)) {
    this->voice_cue_active_.store(false);
    if (this->pa_pin_ >= 0 && !this->announcing_ &&
        (i2s_bridge::I2SBridge::is_active() || !this->player_running_)) {
      gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 0);
    }
  }
  if (this->voice_cue_pending_ && !this->voice_cue_active_.load() &&
      this->codec_if_ && this->i2s_tx_) {
    this->voice_cue_error_ = this->voice_cue_pending_ == 2;
    this->voice_cue_pending_ = 0;
    this->voice_cue_active_.store(true);
    if (xTaskCreate(voice_cue_task_, "voice_cue", 6144, this, 4, nullptr) != pdPASS) {
      ESP_LOGE(TAG, "Failed to create voice cue task");
      this->voice_cue_active_.store(false);
    }
  }

  // Detect WiFi connect
  if (wifi_now && !this->wifi_connected_) {
    this->wifi_connected_ = true;
    if (this->auto_play_pending_ && !this->voice_active_) {
      ESP_LOGI(TAG, "Network connected, resuming stream");
      this->auto_play_pending_ = false;
      this->connect_station_();
      this->publish_station_select_();
    } else {
      ESP_LOGI(TAG, "Network connected, staying idle");
    }
  }
  // Detect WiFi disconnect
  if (!wifi_now && this->wifi_connected_) {
    this->wifi_connected_ = false;
    ESP_LOGW(TAG, "WiFi lost");
  }

  // Retry on stream failure (with debounce to avoid rapid-fire retries)
  if (this->stream_failed_ && !this->player_command_pending_() && this->wifi_connected_ &&
      !this->voice_active_ && !this->announcing_) {
    unsigned long now = millis();
    if (now - this->last_retry_ms_ >= RETRY_INTERVAL_MS) {
      this->last_retry_ms_ = now;
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
          !this->stream_failed_ && !this->announcing_ && !this->voice_active_) {
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
      // Stalled announcement (reply stream never finished): give up so the radio can resume
      if (this->announcing_ && fc == this->watchdog_last_frame_count_ &&
          ++this->announce_stall_count_ >= 10) {
        ESP_LOGW(TAG, "Announcement stalled, aborting");
        this->request_player_(PlayerOperation::STOP);
        this->player_running_ = false;
        this->play_state_ = PS_STOPPED;
        this->announcing_ = false;
        this->announce_pending_ = false;
        this->announce_stall_count_ = 0;
      } else if (!this->announcing_ || fc != this->watchdog_last_frame_count_) {
        this->announce_stall_count_ = 0;
      }
      this->watchdog_last_frame_count_ = fc;
    }
  }

  {
    unsigned long now = millis();
    if (now - this->diag_last_report_ms_ >= 1000) {
      this->diag_last_report_ms_ = now;

      uint32_t pcm_callbacks = this->diag_pcm_callbacks_.exchange(0);
      uint32_t pcm_bytes = this->diag_pcm_bytes_.exchange(0);
      uint32_t written_bytes = this->diag_i2s0_bytes_.exchange(0);
      uint32_t announce_bytes = this->diag_announce_bytes_.exchange(0);
      uint32_t out_peak = this->diag_out_peak_.exchange(0);
      uint32_t i2s0_timeouts = this->diag_i2s0_timeouts_.exchange(0);
      uint32_t i2s0_errors = this->diag_i2s0_errors_.exchange(0);
      uint32_t bridge_timeouts = this->diag_bridge_timeouts_.exchange(0);
      uint32_t bridge_errors = this->diag_bridge_errors_.exchange(0);
      uint32_t bridge_short_writes = this->diag_bridge_short_writes_.exchange(0);
      uint32_t i2s0_short = this->diag_i2s0_short_writes_.exchange(0);
      ESP_LOGI(TAG, "Runtime diag: internal=%u min=%u largest=%u psram=%u loop_us=%lu gap_ms=%lu control=%d reset=%d announce=%d rx=%d voice=%d "
               "http_close=cancel:%lu,%lu stale:%lu,%lu gmf_settle=%lu,%lu,%lu gmf_stop=%lu,%lu",
               unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
               (unsigned long) this->diag_loop_max_us_, (unsigned long) this->diag_loop_gap_max_ms_,
               int(this->player_command_pending_()), int(esp_reset_reason()),
               int(this->announcing_), int(this->rx_enabled_), int(this->voice_active_),
               (unsigned long) this->http_transport_.cancel_close.load(),
               (unsigned long) this->http_transport_.cancel_close_tls.load(),
               (unsigned long) this->http_transport_.stale_close.load(),
               (unsigned long) this->http_transport_.stale_close_tls.load(),
               (unsigned long) this->diag_gmf_settle_waits_.load(),
               (unsigned long) this->diag_gmf_settle_max_ms_.load(),
               (unsigned long) this->diag_gmf_settle_timeouts_.load(),
               (unsigned long) this->diag_gmf_stop_retries_.load(),
               (unsigned long) this->diag_gmf_stop_unconfirmed_.load());
      const size_t failed = failed_alloc_size.exchange(0);
      if (failed)
        ESP_LOGW(TAG, "Allocation failed: size=%u caps=0x%lx internal=%u largest=%u",
                 unsigned(failed), (unsigned long) failed_alloc_caps.load(),
                 unsigned(failed_alloc_internal.load()), unsigned(failed_alloc_largest.load()));
      this->diag_loop_max_us_ = 0;
      this->diag_loop_gap_max_ms_ = 0;

      if (this->play_state_ == PS_PLAYING || pcm_callbacks > 0 ||
          i2s0_timeouts > 0 || i2s0_errors > 0 || bridge_timeouts > 0 ||
          bridge_errors > 0 || bridge_short_writes > 0) {
        ESP_LOGI(
            TAG,
            "Audio diag: cb=%lu bytes=%lu i2s0_to=%lu i2s0_err=%lu "
            "bridge_to=%lu bridge_err=%lu bridge_short=%lu "
            "i2s0_short=%lu sr=%lu bt=%s written=%lu ann=%lu peak=%lu",
            (unsigned long) pcm_callbacks, (unsigned long) pcm_bytes,
            (unsigned long) i2s0_timeouts, (unsigned long) i2s0_errors,
            (unsigned long) bridge_timeouts, (unsigned long) bridge_errors,
            (unsigned long) bridge_short_writes,
            (unsigned long) i2s0_short,
            (unsigned long) this->current_sample_rate_,
            i2s_bridge::I2SBridge::is_active() ? "on" : "off", (unsigned long) written_bytes,
            (unsigned long) announce_bytes, (unsigned long) out_peak);
      }
    }
  }

  // Non-blocking PA enable (replaces 200ms blocking delay)
  // Skip if BT bridge is active — bridge mutes internal speaker
  if (this->pa_pending_ && (millis() - this->pa_pending_ms_ >= 200)) {
    this->pa_pending_ = false;
    if (!i2s_bridge::I2SBridge::is_active() || this->announcing_) {
      gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 1);
    }
  }

  // Debounced NVS volume save
  this->flush_vol_if_dirty_();

  // Publish state changes to HA (reads from stable title buffer)
  const int title_write = 1 - this->title_read_idx_.load(std::memory_order_relaxed);
  if (this->http_transport_.take_title(this->title_bufs_[title_write], sizeof(this->title_bufs_[0])))
    this->title_read_idx_.store(title_write, std::memory_order_release);
  if (this->http_transport_.bitrate() > 0) this->bitrate_ = this->http_transport_.bitrate();
  const char *title = this->title_bufs_[this->title_read_idx_.load(std::memory_order_acquire)];
  bool title_changed = strcmp(title, this->last_published_title_) != 0;
  bool announcing_now = this->announcing_;
  if (this->play_state_ != this->last_published_state_ || title_changed ||
      announcing_now != this->last_published_announcing_) {
    this->update_ha_state_();
  }
  if (this->announce_pending_ && this->player_ &&
      !this->player_command_pending_() &&
      !this->playback_requested_.load()) {
    // Previous announcement finished; start the queued one (announcing_ stayed set)
    this->announce_pending_ = false;
    this->announcing_ = true;  // may have been cleared by a racing FINISHED on Core 0
    this->play_uri_(this->pending_announce_, true);
  }
  // Require the end request to be ~500ms old so a follow-up question can start its mic first
  if (!this->player_command_pending_() && !this->voice_cue_pending_ && !this->voice_cue_active_.load() &&
      millis() - this->voice_end_ms_ >= 500 && voice_logic::should_restore(this->voice_end_pending_, this->announcing_,
                                  this->announce_pending_, this->mic_requested_,
                                  this->rx_enabled_)) {
    this->restore_after_voice_();
  }
  if (title_changed && this->now_playing_sensor_) {
    this->now_playing_sensor_->publish_state(title);
  }
  const char *phase = this->get_playback_phase();
  if (this->playback_phase_sensor_ &&
      (!this->last_playback_phase_ || strcmp(phase, this->last_playback_phase_) != 0)) {
    this->last_playback_phase_ = phase;
    this->playback_phase_sensor_->publish_state(phase);
  }
  this->diag_loop_max_us_ = std::max(this->diag_loop_max_us_, uint32_t(micros() - loop_start));
}

// ─── Audio init (Core 1, called from setup) ──────────────

void InternetRadio::init_i2s0_() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  // The ESP-GMF decoder delivers PCM in bursts: measured callback gaps reach
  // ~42ms while the default DMA depth (6*240 frames ≈ 33ms @44.1k) is smaller,
  // so the DMA underflows on jitter spikes and auto_clear injects silence
  // (audible micro-stutter). Enlarge to 8*512 frames ≈ 93ms to absorb jitter.
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 512;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &this->i2s_tx_, &this->i2s_rx_));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = (gpio_num_t)this->mclk_pin_,
          .bclk = (gpio_num_t)this->bclk_pin_,
          .ws = (gpio_num_t)this->lrclk_pin_,
          .dout = (gpio_num_t)this->dout_pin_,
          .din = (gpio_num_t)this->din_pin_,
          .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
      },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  // Full duplex requires identical TX/RX configs (including gpio_cfg)
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(this->i2s_tx_, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(this->i2s_rx_, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(this->i2s_tx_));

  this->current_sample_rate_ = 44100;
  ESP_LOGI(TAG, "I2S0: BCLK=%d LRCK=%d DOUT=%d DIN=%d MCLK=%d (44100Hz duplex), free internal heap=%u",
           this->bclk_pin_, this->lrclk_pin_, this->dout_pin_, this->din_pin_, this->mclk_pin_,
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void InternetRadio::init_es8311_() {
  // Get ESPHome's I2C bus handle
  i2c_master_bus_handle_t i2c_bus = nullptr;
  esp_err_t err = i2c_master_get_bus_handle(0, &i2c_bus);
  if (err != ESP_OK || !i2c_bus) {
    ESP_LOGW(TAG, "I2C bus 0 handle: err=%s handle=%p", esp_err_to_name(err), i2c_bus);
    return;
  }

  // I2C control interface for ES8311
  // NOTE: esp_codec_dev uses 8-bit I2C address (7-bit << 1). ES8311 = 0x18 << 1 = 0x30
  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = 0,
      .addr = 0x30,
      .bus_handle = i2c_bus,
  };
  const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
  if (!ctrl) {
    ESP_LOGE(TAG, "Failed to create I2C ctrl interface");
    return;
  }

  const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();

  es8311_codec_cfg_t es_cfg = {};
  es_cfg.ctrl_if = ctrl;
  es_cfg.gpio_if = gpio;
  es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  es_cfg.pa_pin = -1;  // We manage PA ourselves (GPIO46)
  es_cfg.use_mclk = true;
  es_cfg.master_mode = false;
  es_cfg.digital_mic = false;
  es_cfg.invert_mclk = false;
  es_cfg.invert_sclk = false;

  // Single attempt — caller (loop) handles retry cadence
  this->codec_if_ = es8311_codec_new(&es_cfg);
  if (!this->codec_if_) {
    ESP_LOGW(TAG, "ES8311 codec not ready, will retry...");
    audio_codec_delete_ctrl_if(ctrl);
    audio_codec_delete_gpio_if(gpio);
    return;
  }

  // es8311_codec_new() already calls open() internally.
  // Enable DAC output and configure for current stream sample rate.
  this->codec_if_->enable(this->codec_if_, true);

  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = this->current_sample_rate_;
  fs.channel = 2;
  fs.bits_per_sample = 16;
  fs.mclk_multiple = 256;
  this->codec_if_->set_fs(this->codec_if_, &fs);

  // ES8311 DAC at +6 dB — analog boost for internal speaker
  this->codec_if_->set_vol(this->codec_if_, 6.0f);
  this->sw_vol_gain_ = this->is_muted_ ? 0 : map_vol_q8_(this->vol_);
  ESP_LOGI(TAG, "ES8311: vol_step=%d q8=%d", (int)this->vol_, (int)this->sw_vol_gain_);

  ESP_LOGI(TAG, "ES8311 initialized (%luHz stereo 16-bit)",
           (unsigned long)this->current_sample_rate_);
}

void InternetRadio::init_player_() {
  gmf_audio::initialize_codec_memory();
  esp_asp_cfg_t cfg = {};
  cfg.out.cb = pcm_output_cb_;
  cfg.out.user_ctx = this;
  cfg.task_prio = 5;
  cfg.task_stack = 8 * 1024;
  cfg.task_core = 0;
  cfg.task_stack_in_ext = true;  // use PSRAM for stack

  esp_gmf_err_t ret = esp_audio_simple_player_new(&cfg, &this->player_);
  if (ret != ESP_GMF_ERR_OK || !this->player_) {
    ESP_LOGE(TAG, "Failed to create ESP-GMF player: %d", ret);
    return;
  }

  esp_audio_simple_player_set_event(this->player_, player_event_cb_, this);

  // Register custom HTTP IO with cert bundle (built-in HTTP IO is disabled)
  this->init_http_io_();
  this->player_commands_ = xQueueCreate(1, sizeof(PlayerCommand));
  if (!this->player_commands_ ||
      xTaskCreatePinnedToCore(player_control_task_, "radio_control", 6144, this, 6, nullptr, 0) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create player control worker");
    this->player_control_fault_.store(true);
    if (this->player_commands_) vQueueDelete(this->player_commands_);
    this->player_commands_ = nullptr;
  }

  ESP_LOGI(TAG, "ESP-GMF player created (Core 0, 8KB stack)");
}

void InternetRadio::init_http_io_() {
  http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
  http_cfg.dir = ESP_GMF_IO_DIR_READER;
  http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  http_cfg.event_handle = gmf_audio::HttpTransport::event;
  http_cfg.user_data = &this->http_transport_;

  // Enlarge the upstream network→decoder ring buffer (allocated in PSRAM).
  // The GMF default (20KB ≈ 1.25s @128kbps) starves the decoder during the
  // ~200ms delivery stalls seen on high-latency links to distant (e.g. US)
  // stream servers, causing random audible glitches. 256KB gives more
  // jitter headroom. Keep io_size at the GMF default (3KB): a larger read
  // chunk makes the decoder block longer per acquire and worsens gaps.
  http_cfg.io_cfg.buffer_cfg.buffer_size = 256 * 1024;

  esp_gmf_err_t ret = esp_gmf_io_http_init(&http_cfg, &this->http_io_);
  if (ret != ESP_GMF_ERR_OK) {
    ESP_LOGE(TAG, "Failed to create HTTP IO: %d", ret);
    return;
  }
  esp_audio_simple_player_register_io(this->player_, this->http_io_);
  ESP_LOGI(TAG, "HTTP IO registered (HTTPS cert bundle enabled)");
}

// ─── PCM output callback (Core 0, ESP-GMF worker thread) ─

int InternetRadio::pcm_output_cb_(uint8_t *data, int size, void *ctx) {
  auto *self = static_cast<InternetRadio *>(ctx);
  if (!self->accept_player_events_.load() || !self->pcm_enabled_.load()) {
    // Discarding PCM removes the normal I2S wait. Yield during teardown so a
    // high-priority decoder cannot starve the control worker or Core 0 idle.
    vTaskDelay(1);
    return size;
  }
  size_t written = 0;
  self->diag_pcm_callbacks_++;
  self->diag_pcm_bytes_ += size;

  // 1. Feed FFT BEFORE volume scaling — visualizer needs full-amplitude PCM
  feed_fft_samples(data, size);

  // 2. Software volume: scale 16-bit PCM samples using Q8 fixed-point gain
  const bool announcement = self->announcement_output_.load();
  int gain = voice_logic::playback_gain(self->sw_vol_gain_, false, announcement, VOICE_REPLY_GAIN_Q8);
  if (gain != 256) {
    voice_logic::apply_gain_q8(reinterpret_cast<int16_t *>(data), size / 2, gain);
  }
  if (announcement) self->diag_announce_bytes_ += size;
  // Post-gain peak proves audibility (announcement while muted) or silence.
  const uint32_t peak = voice_logic::peak_abs(reinterpret_cast<const int16_t *>(data), size / 2);
  if (peak > self->diag_out_peak_.load(std::memory_order_relaxed))
    self->diag_out_peak_.store(peak, std::memory_order_relaxed);

  // 3. Write to ES8311 speaker (bounded timeout to avoid hanging stop/reconnect)
  esp_err_t err = ESP_OK;
  bool wrote_pcm = false;
  written = size;
  if (!self->voice_cue_active_.load()) {
    std::lock_guard<std::mutex> lock(self->i2s_cfg_mutex_);
    if (!self->voice_cue_active_.load()) {
      err = i2s_channel_write(self->i2s_tx_, data, size, &written, pdMS_TO_TICKS(100));
      wrote_pcm = written > 0;
      self->diag_i2s0_bytes_ += written;
    }
  }
  if (err == ESP_ERR_TIMEOUT) {
    self->diag_i2s0_timeouts_++;
  } else if (err != ESP_OK) {
    self->diag_i2s0_errors_++;
    return -1;
  }
  if (written < (size_t)size) self->diag_i2s0_short_writes_++;

  // 4. Write to I2S bridge (BT speaker) if active — voice replies stay on the internal speaker
  if (!self->announcement_output_.load()) self->write_bridge_pcm_(data, size);

  // 5. Watchdog frame count
  if (wrote_pcm) g_audio_frame_count++;
  if (wrote_pcm && err == ESP_OK) {
    std::lock_guard<std::mutex> request_lock(self->player_request_mutex_);
    if (self->pcm_enabled_.load() && self->accept_player_events_.load()) {
      self->pcm_started_.store(true);
      self->play_state_ = PS_PLAYING;
    }
  }

  return (int)written;
}

void InternetRadio::write_bridge_pcm_(const uint8_t *data, int size) {
  i2s_chan_handle_t bridge_tx = i2s_bridge::I2SBridge::get_tx_handle();
  if (!i2s_bridge::I2SBridge::is_active() || bridge_tx == nullptr || size <= 0) {
    return;
  }

  const uint32_t in_rate = this->current_sample_rate_;
  const uint8_t *out_data = data;
  int out_size = size;

  if (in_rate != BRIDGE_OUTPUT_RATE && in_rate > 0) {
    const int16_t *in = reinterpret_cast<const int16_t *>(data);
    int in_frames = size / (2 * sizeof(int16_t));
    if (in_frames <= 0) {
      return;
    }

    if (!this->bridge_resample_have_prev_) {
      this->bridge_resample_prev_l_ = in[0];
      this->bridge_resample_prev_r_ = in[1];
      this->bridge_resample_pos_q16_ = 1u << 16;
      this->bridge_resample_have_prev_ = true;
    }

    uint32_t step_q16 = (uint32_t) (((uint64_t) in_rate << 16) / BRIDGE_OUTPUT_RATE);
    if (step_q16 == 0) {
      step_q16 = 1;
    }

    int out_frames = 0;
    uint32_t end_q16 = (uint32_t) in_frames << 16;
    while (this->bridge_resample_pos_q16_ < end_q16 &&
           out_frames < BRIDGE_RESAMPLE_MAX_FRAMES) {
      uint32_t idx = this->bridge_resample_pos_q16_ >> 16;
      uint32_t frac = this->bridge_resample_pos_q16_ & 0xFFFFu;

      int16_t al;
      int16_t ar;
      if (idx == 0) {
        al = this->bridge_resample_prev_l_;
        ar = this->bridge_resample_prev_r_;
      } else {
        al = in[(idx - 1) * 2];
        ar = in[(idx - 1) * 2 + 1];
      }
      int16_t bl = in[idx * 2];
      int16_t br = in[idx * 2 + 1];

      int32_t dl = (int32_t) bl - al;
      int32_t dr = (int32_t) br - ar;
      this->bridge_resample_buf_[out_frames * 2] =
          (int16_t) (al + ((dl * (int32_t) frac) >> 16));
      this->bridge_resample_buf_[out_frames * 2 + 1] =
          (int16_t) (ar + ((dr * (int32_t) frac) >> 16));

      this->bridge_resample_pos_q16_ += step_q16;
      out_frames++;
    }

    if (out_frames >= BRIDGE_RESAMPLE_MAX_FRAMES &&
        this->bridge_resample_pos_q16_ < end_q16) {
      this->diag_bridge_short_writes_++;
    }

    this->bridge_resample_pos_q16_ -= end_q16;
    this->bridge_resample_prev_l_ = in[(in_frames - 1) * 2];
    this->bridge_resample_prev_r_ = in[(in_frames - 1) * 2 + 1];

    out_data = reinterpret_cast<const uint8_t *>(this->bridge_resample_buf_);
    out_size = out_frames * 2 * sizeof(int16_t);
  } else {
    this->bridge_resample_have_prev_ = false;
    this->bridge_resample_pos_q16_ = 1u << 16;
  }

  if (out_size <= 0) {
    return;
  }

  size_t bw = 0;
  esp_err_t bridge_err = i2s_channel_write(bridge_tx, out_data, out_size, &bw, 0);
  if (bridge_err == ESP_ERR_TIMEOUT) {
    this->diag_bridge_timeouts_++;
  } else if (bridge_err != ESP_OK) {
    this->diag_bridge_errors_++;
  }
  if (bw < (size_t) out_size) {
    this->diag_bridge_short_writes_++;
  }
}

// ─── Player event callback (Core 0, ESP-GMF worker thread)

int InternetRadio::player_event_cb_(esp_asp_event_pkt_t *pkt, void *ctx) {
  auto *self = static_cast<InternetRadio *>(ctx);
  if (pkt->type == ESP_ASP_EVENT_TYPE_STATE) {
    // Also for superseded runs: this callback runs on GMF's thread before it winds down.
    const esp_asp_state_t state = *(esp_asp_state_t *)pkt->payload;
    if (state == ESP_ASP_STATE_ERROR || state == ESP_ASP_STATE_FINISHED) {
      self->gmf_task_.store(xTaskGetCurrentTaskHandle());
      self->gmf_settle_pending_.store(true);
    }
    if (state == ESP_ASP_STATE_ERROR || state == ESP_ASP_STATE_FINISHED || state == ESP_ASP_STATE_STOPPED)
      self->gmf_ended_.store(true);
  }
  if (!self->accept_player_events_.load()) return 0;

  if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO) {
    esp_asp_music_info_t *info = (esp_asp_music_info_t *)pkt->payload;
    ESP_LOGI(TAG, "Stream: %d Hz, %d ch, %d bit, %d kbps",
             info->sample_rate, info->channels, info->bits, info->bitrate);

    if (info->channels != 2 || info->bits != 16) {
      ESP_LOGW(TAG, "Unexpected format: %d ch %d bit (expected stereo 16-bit)",
               info->channels, info->bits);
    }

    // Reconfigure I2S0 if sample rate changed
    if ((uint32_t)info->sample_rate != self->current_sample_rate_ &&
        info->sample_rate > 0) {
      self->reconfig_sample_rate_((uint32_t)info->sample_rate);
      self->bridge_resample_have_prev_ = false;
      self->bridge_resample_pos_q16_ = 1u << 16;
    }

    if (info->bitrate > 0) {
      self->bitrate_ = info->bitrate;
    }
    i2s_bridge::I2SBridge::set_sample_rate((uint32_t) info->sample_rate);
  }

  if (pkt->type == ESP_ASP_EVENT_TYPE_STATE) {
    std::lock_guard<std::mutex> request_lock(self->player_request_mutex_);
    if (!self->accept_player_events_.load()) return 0;
    esp_asp_state_t state = *(esp_asp_state_t *)pkt->payload;
    ESP_LOGI(TAG, "Player state: %d", (int)state);
    switch (state) {
      case ESP_ASP_STATE_RUNNING:
        self->player_running_ = true;
        // Enable PA (deferred) — only if BT bridge isn't active
        if (self->pa_pin_ >= 0 && !self->pa_pending_ &&
            (!i2s_bridge::I2SBridge::is_active() || self->announcing_)) {
          self->pa_pending_ = true;
          self->pa_pending_ms_ = millis();
        }
        break;
      case ESP_ASP_STATE_PAUSED:
        self->play_state_ = PS_PAUSED;
        break;
      case ESP_ASP_STATE_STOPPED:
        self->play_state_ = PS_STOPPED;
        self->player_running_ = false;
        self->playback_requested_.store(false);
        break;
      case ESP_ASP_STATE_FINISHED:
      case ESP_ASP_STATE_ERROR:
        self->playback_requested_.store(false);
        if (self->announcing_) {
          // TTS reply done: not a stream failure; Core 1 restores the radio.
          // Keep announcing_ set if another announcement is queued.
          if (!self->announce_pending_) self->announcing_ = false;
          self->play_state_ = PS_STOPPED;
          self->player_running_ = false;
          break;
        }
        ESP_LOGW(TAG, "Stream %s", state == ESP_ASP_STATE_ERROR ? "error" : "finished");
        self->stream_failed_ = true;
        self->play_state_ = PS_STOPPED;
        self->player_running_ = false;
        break;
      default:
        break;
    }
  }

  return 0;
}

void InternetRadio::reconfig_sample_rate_(uint32_t new_rate) {
  ESP_LOGI(TAG, "Reconfiguring I2S0: %lu → %lu Hz",
           (unsigned long)this->current_sample_rate_, (unsigned long)new_rate);

  std::lock_guard<std::mutex> lock(this->i2s_cfg_mutex_);
  bool rx_was_on = this->rx_enabled_;
  if (rx_was_on) i2s_channel_disable(this->i2s_rx_);
  i2s_channel_disable(this->i2s_tx_);

  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(new_rate);
  clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(this->i2s_tx_, &clk));
  esp_err_t rx_err = i2s_channel_reconfig_std_clock(this->i2s_rx_, &clk);
  if (rx_err != ESP_OK) ESP_LOGE(TAG, "RX clock reconfig failed: %s", esp_err_to_name(rx_err));
  ESP_ERROR_CHECK(i2s_channel_enable(this->i2s_tx_));
  if (rx_was_on) {
    rx_err = i2s_channel_enable(this->i2s_rx_);
    if (rx_err != ESP_OK) {
      ESP_LOGE(TAG, "RX re-enable failed: %s", esp_err_to_name(rx_err));
      this->rx_enabled_ = false;
    }
  }

  // Update ES8311 sample rate
  if (this->codec_if_) {
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = new_rate;
    fs.channel = 2;
    fs.bits_per_sample = 16;
    fs.mclk_multiple = 256;
    this->codec_if_->set_fs(this->codec_if_, &fs);
  }

  this->current_sample_rate_ = new_rate;
}

bool InternetRadio::request_player_(PlayerOperation operation, const char *uri, bool announcement) {
  std::lock_guard<std::mutex> request_lock(this->player_request_mutex_);
  if (!this->player_commands_ || this->player_control_fault_.load()) {
    ESP_LOGE(TAG, "Player control unavailable; request rejected");
    return false;
  }
  PlayerCommand command{};
  command.operation = operation;
  command.announcement = announcement;
  if (uri) {
    if (strlen(uri) >= sizeof(command.uri) - 6) {
      ESP_LOGE(TAG, "Media URI too long; request rejected");
      return false;
    }
    const char *ext = strrchr(uri, '.');
    if (!ext || strchr(ext, '/'))
      snprintf(command.uri, sizeof(command.uri), "%s#.aac", uri);
    else
      strlcpy(command.uri, uri, sizeof(command.uri));
  }
  if (operation == PlayerOperation::STOP || operation == PlayerOperation::RUN) {
    this->accept_player_events_.store(false);
    this->play_state_ = PS_STOPPED;
    this->pa_pending_ = false;
    this->stream_failed_ = false;
    if (operation == PlayerOperation::RUN) {
      this->announcing_ = announcement;
      this->bitrate_ = 0;
    }
    this->playback_requested_.store(operation == PlayerOperation::RUN);
    this->playback_launched_.store(false);
    this->pcm_started_.store(false);
    // Cancels the HTTP reader of the running stream before the worker even wakes.
    command.stream_epoch = this->stream_epoch_.fetch_add(1) + 1;
    this->http_transport_.set_epoch(command.stream_epoch);
  } else if (operation == PlayerOperation::PAUSE) {
    // Record the paused intent now: a voice session or announcement arriving before
    // the worker applies the PAUSE must not resume the radio afterwards.
    this->playback_requested_.store(false);
    this->play_state_ = PS_PAUSED;
  } else {
    this->playback_requested_.store(true);
  }
  if (operation != PlayerOperation::RESUME) this->pcm_enabled_.store(false);
  if (!this->player_command_pending_()) this->player_command_ms_ = millis();
  command.generation = this->player_generation_.fetch_add(1) + 1;
  xQueueOverwrite(this->player_commands_, &command);
  return true;
}

// After a failed open, GMF reports ERROR from its own thread and only then deletes the
// task's job list. A RUN registered in between loses its jobs and times out after 5 s
// ("Run timeout"). Wait until that thread is parked again (blocked waiting for a run).
void InternetRadio::wait_gmf_parked_() {
  if (!this->gmf_settle_pending_.exchange(false)) return;
  TaskHandle_t task = this->gmf_task_.load();
  if (task == nullptr) return;
  const uint32_t start = millis();
  int parked = 0;
  while (parked < 3 && millis() - start < 500) {
    const eTaskState state = eTaskGetState(task);
    parked = (state == eBlocked || state == eSuspended) ? parked + 1 : 0;
    if (parked < 3) vTaskDelay(1);
  }
  const uint32_t ms = millis() - start;
  this->diag_gmf_settle_waits_++;
  if (ms > this->diag_gmf_settle_max_ms_.load()) this->diag_gmf_settle_max_ms_.store(ms);
  if (parked < 3) {
    this->diag_gmf_settle_timeouts_++;
    ESP_LOGW(TAG, "GMF task still busy %lu ms after its run ended", (unsigned long) ms);
  }
}

// GMF sets the task's STOP bit when a run ends by itself (ERROR/FINISHED or failed
// prepare) and clears it only once the next run reaches its job loop. A stop issued while
// that next run is still opening its input consumes the stale bit and returns OK at once,
// while the pipeline keeps running; the following RUN then times out after 5 s. A real
// stop always reports STOPPED/FINISHED/ERROR before it returns, so stop again if not.
esp_gmf_err_t InternetRadio::stop_pipeline_(bool launched) {
  esp_gmf_err_t result = esp_audio_simple_player_stop(this->player_);
  if (result == ESP_GMF_ERR_OK && launched && !this->gmf_ended_.load()) {
    this->diag_gmf_stop_retries_++;
    ESP_LOGW(TAG, "GMF stop returned while the run was still opening; stopping again");
    result = esp_audio_simple_player_stop(this->player_);
    if (result == ESP_GMF_ERR_OK && !this->gmf_ended_.load()) {
      this->diag_gmf_stop_unconfirmed_++;
      ESP_LOGE(TAG, "GMF stop not confirmed by a terminal pipeline event");
    }
  }
  return result;
}

static const char *player_op_name(int op) {
  static const char *const names[] = {"STOP", "RUN", "PAUSE", "RESUME"};
  return op >= 0 && op < 4 ? names[op] : "?";
}

void InternetRadio::player_control_task_(void *arg) {
  auto *self = static_cast<InternetRadio *>(arg);
  PlayerCommand command{};
  playback_logic::PipelineStopTracker pipeline;
  bool launched = false;  // the last run returned OK and has not been stopped since
  for (;;) {
    if (xQueueReceive(self->player_commands_, &command, portMAX_DELAY) != pdTRUE) continue;
    const uint32_t start = millis();
    esp_gmf_err_t result = ESP_GMF_ERR_OK;
    if (command.operation == PlayerOperation::RUN || command.operation == PlayerOperation::STOP) {
      self->accept_player_events_.store(false);
      self->wait_gmf_parked_();
      // Stop even after ERROR: the HTTP/decoder workers can still own resources.
      // Never stop twice in a row (superseded RUN): see PipelineStopTracker.
      if (pipeline.needs_stop()) {
        result = self->stop_pipeline_(launched);
        pipeline.stop_result(result == ESP_GMF_ERR_OK);
        launched = false;
      }
      self->player_running_ = false;
      if (command.announcement) {
        // Wake capture winds down in Box3Mic::loop(); never steal an active RX reader.
        const uint32_t rx_wait = millis();
        while (self->rx_enabled_ && millis() - rx_wait < 2000 &&
               command.generation == self->player_generation_.load())
          vTaskDelay(pdMS_TO_TICKS(10));
        if (self->rx_enabled_) result = ESP_GMF_ERR_TIMEOUT;
      }
      bool run = false;
      {
        std::lock_guard<std::mutex> request_lock(self->player_request_mutex_);
        if (result == ESP_GMF_ERR_OK &&
            command.generation == self->player_generation_.load() &&
            !self->player_control_fault_.load()) {
          self->announcement_output_.store(command.announcement);
          if (command.operation == PlayerOperation::RUN) {
            self->announcing_ = command.announcement;
            self->pcm_enabled_.store(true);
            self->accept_player_events_.store(true);
            self->http_transport_.launch(command.stream_epoch);
            // Startup clock starts at the actual launch, not at request time.
            self->playback_start_ms_.store(millis());
            self->playback_launched_.store(true);
            run = true;
          } else {
            self->announcing_ = false;
            self->play_state_ = PS_STOPPED;
            self->playback_requested_.store(false);
          }
        }
      }
      if (run) {
        self->gmf_ended_.store(false);
        result = esp_audio_simple_player_run(self->player_, command.uri, nullptr);
        launched = result == ESP_GMF_ERR_OK;
        esp_gmf_pipeline_handle_t pipe = nullptr;
        pipeline.run_attempted(esp_audio_simple_player_get_pipeline(self->player_, &pipe) == ESP_GMF_ERR_OK &&
                               pipe != nullptr);
      }
    } else if (command.generation == self->player_generation_.load()) {
      self->pcm_enabled_.store(command.operation == PlayerOperation::RESUME);
      result = command.operation == PlayerOperation::PAUSE
                   ? esp_audio_simple_player_pause(self->player_)
                   : esp_audio_simple_player_resume(self->player_);
    }
    if (result != ESP_GMF_ERR_OK) {
      const playback_logic::FailedOp failed_op =
          command.operation == PlayerOperation::RUN     ? playback_logic::FailedOp::RUN
          : command.operation == PlayerOperation::PAUSE  ? playback_logic::FailedOp::PAUSE
          : command.operation == PlayerOperation::RESUME ? playback_logic::FailedOp::RESUME
                                                         : playback_logic::FailedOp::STOP;
      bool cleanup_stop = false;
      {
        std::lock_guard<std::mutex> request_lock(self->player_request_mutex_);
        if (command.generation == self->player_generation_.load()) {
          const bool announcement = command.announcement || self->announcement_output_.load();
          ESP_LOGE(TAG, "Player %s failed: %d", player_op_name(int(command.operation)), result);
          self->accept_player_events_.store(false);
          self->pcm_enabled_.store(false);
          self->announcing_ = false;
          self->play_state_ = PS_STOPPED;
          self->playback_requested_.store(false);
          self->playback_launched_.store(false);
          self->stream_failed_ = playback_logic::failure_retries(failed_op, announcement);
          if (playback_logic::needs_cleanup_stop(failed_op)) {
            // GMF state is unknown after a failed pause/resume: fence the HTTP reader
            // and stop the pipeline here, still inside this serialized command.
            self->http_transport_.set_epoch(self->stream_epoch_.fetch_add(1) + 1);
            cleanup_stop = true;
          }
        }
      }
      if (cleanup_stop && pipeline.needs_stop()) {
        const esp_gmf_err_t stop_result = self->stop_pipeline_(launched);
        pipeline.stop_result(stop_result == ESP_GMF_ERR_OK);
        launched = false;
        self->player_running_ = false;
        if (stop_result == ESP_GMF_ERR_OK)
          ESP_LOGW(TAG, "Pipeline stopped after failed %s", player_op_name(int(command.operation)));
        else
          ESP_LOGE(TAG, "Cleanup stop after failed %s also failed: %d", player_op_name(int(command.operation)),
                   stop_result);
      }
    }
    ESP_LOGI(TAG, "Player control: generation=%lu op=%d ms=%lu result=%d stack=%u",
             (unsigned long) command.generation, int(command.operation),
             (unsigned long) (millis() - start), result, unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    self->player_completed_.store(command.generation);
  }
}

bool InternetRadio::play_uri_(const char *url, bool announcement) {
  if (this->request_player_(PlayerOperation::RUN, url, announcement)) return true;
  this->announcing_ = false;
  return false;
}

// ─── Voice assistant support ──────────────────────────────

void InternetRadio::play_voice_cue(bool error) {
  // Errors replace a queued listening cue; repeated listening events coalesce.
  if (error || this->voice_cue_pending_ == 0) this->voice_cue_pending_ = error ? 2 : 1;
}

void InternetRadio::voice_cue_task_(void *arg) {
  auto *self = static_cast<InternetRadio *>(arg);
  {
    std::lock_guard<std::mutex> lock(self->i2s_cfg_mutex_);
    const uint32_t rate = self->current_sample_rate_;
    const uint32_t frames = rate * (self->voice_cue_error_ ? 200 : 120) / 1000;
    int16_t pcm[256 * 2];
    if (self->pa_pin_ >= 0) {
      gpio_set_level(static_cast<gpio_num_t>(self->pa_pin_), 1);
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (uint32_t pos = 0; pos < frames;) {
      const uint32_t count = std::min(uint32_t(256), frames - pos);
      for (uint32_t i = 0; i < count; i++) {
        pcm[2 * i] = pcm[2 * i + 1] = voice_logic::cue_sample(pos + i, rate, self->voice_cue_error_);
      }
      size_t written = 0;
      const size_t bytes = count * 2 * sizeof(int16_t);
      esp_err_t err = i2s_channel_write(self->i2s_tx_, pcm, bytes, &written, 100);
      if (err != ESP_OK || written != bytes) {
        ESP_LOGE(TAG, "Voice cue write failed: %s (%u/%u bytes)",
                 esp_err_to_name(err), unsigned(written), unsigned(bytes));
        break;
      }
      pos += count;
    }
    // Let the DMA drain before the loop restores the speaker routing.
    vTaskDelay(pdMS_TO_TICKS(4096 * 1000 / rate + 10));
  }
  self->voice_cue_done_.store(true);
  vTaskDelete(nullptr);
}

int InternetRadio::voice_capture_begin() {
  if (this->i2s_rx_ == nullptr) return 0;
  // A new capture cancels any pending restore (follow-up questions)
  this->voice_end_pending_ = false;
  this->announce_pending_ = false;
  if (!this->voice_active_) {
    this->voice_active_ = true;
    this->voice_resume_radio_ = playback_logic::resume_after_voice(
        int(this->play_state_), this->playback_requested_.load(), this->stream_failed_);
    this->stream_failed_ = false;
  }
  // Always stop the player (radio or leftover announcement): the mic needs the clock
  // Stopping the player can block ~3 s (HTTP teardown). Pause it (silences I2S0 TX at
  // once) and finish the stop on a worker task while the mic is already listening.
  if (this->player_control_fault_.load()) return 0;
  if (this->player_command_pending_()) return -1;
  if (this->player_running_ || this->accept_player_events_.load()) {
    if (!this->request_player_(PlayerOperation::STOP)) return 0;
    return -1;
  }
  this->announcing_ = false;
  if (this->current_sample_rate_ != 16000) this->reconfig_sample_rate_(16000);
  {
    std::lock_guard<std::mutex> lock(this->i2s_cfg_mutex_);
    if (!this->rx_enabled_) {
      esp_err_t err = i2s_channel_enable(this->i2s_rx_);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX enable failed: %s", esp_err_to_name(err));
        this->voice_end_pending_ = true;
        return 0;
      }
      this->rx_enabled_ = true;
    }
  }
  ESP_LOGI(TAG, "Voice capture started (radio resume=%d)", (int) this->voice_resume_radio_);
  return 1;
}

int InternetRadio::wake_capture_begin() {
  if (this->i2s_rx_ == nullptr) return 0;
  if (this->voice_active_ || this->voice_end_pending_) return -1;
  std::lock_guard<std::mutex> lock(this->i2s_cfg_mutex_);
  if (!this->rx_enabled_) {
    esp_err_t err = i2s_channel_enable(this->i2s_rx_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Wake RX enable failed: %s", esp_err_to_name(err));
      return 0;
    }
    this->rx_enabled_ = true;
  }
  return 1;
}

void InternetRadio::voice_capture_end() {
  std::lock_guard<std::mutex> lock(this->i2s_cfg_mutex_);
  if (this->rx_enabled_) {
    i2s_channel_disable(this->i2s_rx_);
    this->rx_enabled_ = false;
  }
}

void InternetRadio::voice_session_end() {
  // Restore happens in loop() once any TTS announcement has finished
  this->voice_end_ms_ = millis();
  this->voice_end_pending_ = true;
}

void InternetRadio::restore_after_voice_() {
  this->voice_end_pending_ = false;
  if (!this->voice_active_) return;
  this->voice_active_ = false;
  this->voice_capture_end();
  // Internal speaker was used for the reply; mute it again if BT owns the output
  if (this->pa_pin_ >= 0 && i2s_bridge::I2SBridge::is_active()) {
    this->pa_pending_ = false;
    gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 0);
  }
  if (this->has_pending_media_) {
    this->has_pending_media_ = false;
    this->voice_resume_radio_ = false;
    ESP_LOGI(TAG, "Voice session over, playing deferred URL");
    this->play_uri_(this->pending_media_);
    if (this->player_command_pending_()) this->save_resume_playback_(true);
  } else if (this->voice_resume_radio_) {
    this->voice_resume_radio_ = false;
    ESP_LOGI(TAG, "Voice session over, resuming station");
    this->connect_station_();
  }
  this->watchdog_stall_count_ = 0;
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
  // Announcements (voice assistant TTS) are decoded by the same pipeline;
  // the PCM path is stereo-only, so request stereo MP3 at 44.1 kHz. sample_bytes
  // stays 0: HA's ffmpeg fails (HTTP 500) when mp3 is combined with 16-bit samples.
  traits.get_supported_formats().push_back(
      {"mp3", 44100, 2, media_player::MediaPlayerFormatPurpose::PURPOSE_ANNOUNCEMENT, 0});
  return traits;
}

void InternetRadio::control(const media_player::MediaPlayerCall &call) {
  if (call.get_volume().has_value()) {
    float v = *call.get_volume();
    this->set_volume_direct((int)(v * 21.0f + 0.5f));
  }

  if (call.get_media_url().has_value()) {
    const std::string &url = *call.get_media_url();
    bool announce = call.get_announcement().value_or(false);
    ESP_LOGI(TAG, "Play URL%s: %s", announce ? " (announcement)" : "", url.c_str());
    bool enqueue = call.get_command().has_value() &&
                   *call.get_command() == media_player::MEDIA_PLAYER_COMMAND_ENQUEUE;
    if (announce && enqueue && this->announcing_) {
      // Queue behind the current announcement (e.g. preannounce chime, then TTS)
      strlcpy(this->pending_announce_, url.c_str(), sizeof(this->pending_announce_));
      this->announce_pending_ = true;
    } else if (announce) {
      // TTS reply: leave the station title and resume flag untouched
      if (!this->voice_active_) {
        // Standalone announcement: behave as a mini session so the radio resumes after
        this->voice_active_ = true;
        this->voice_resume_radio_ = playback_logic::resume_after_voice(
            int(this->play_state_), this->playback_requested_.load(), false);
        this->voice_end_ms_ = millis();
        this->voice_end_pending_ = true;
      }
      this->announce_pending_ = false;
      this->play_uri_(url.c_str(), true);
    } else {
      if (this->voice_active_) {
        // Mic/TTS owns I2S0; play the requested URL once the session ends
        strlcpy(this->pending_media_, url.c_str(), sizeof(this->pending_media_));
        this->has_pending_media_ = true;
        this->voice_resume_radio_ = false;
        this->update_ha_state_();
        return;
      }
      this->announcing_ = false;
      this->announce_pending_ = false;
      this->id3_artist_[0] = '\0';
      this->id3_title_[0] = '\0';
      int write_idx = 1 - this->title_read_idx_.load(std::memory_order_relaxed);
      strlcpy(this->title_bufs_[write_idx], "Connecting...", 128);
      this->title_read_idx_.store(write_idx, std::memory_order_release);
      this->play_uri_(url.c_str());
      if (this->player_command_pending_()) this->save_resume_playback_(true);
    }
    this->watchdog_stall_count_ = 0;
  }

  if (call.get_command().has_value()) {
    ESP_LOGI(TAG, "Command received: %d", (int)*call.get_command());
    switch (*call.get_command()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        if (this->play_state_ == PS_PAUSED && this->player_) {
          this->request_player_(PlayerOperation::RESUME);
          this->save_resume_playback_(true);
        } else if (this->play_state_ == PS_STOPPED) {
          this->connect_station_();
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
        if (this->voice_active_) { this->voice_resume_radio_ = false; this->has_pending_media_ = false; }
        if (this->play_state_ == PS_PLAYING && this->player_) {
          this->request_player_(PlayerOperation::PAUSE);
          this->save_resume_playback_(false);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        if (this->voice_active_) { this->voice_resume_radio_ = false; this->has_pending_media_ = false; }
        if (this->player_) {
          this->request_player_(PlayerOperation::STOP);
          this->player_running_ = false;
          this->play_state_ = PS_STOPPED;
          this->save_resume_playback_(false);
          this->announcing_ = false;
          this->announce_pending_ = false;
          if (this->pa_pin_ >= 0 && !i2s_bridge::I2SBridge::is_active())
            gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 0);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (this->play_state_ == PS_PLAYING && this->player_) {
          this->request_player_(PlayerOperation::PAUSE);
          this->save_resume_playback_(false);
        } else if (this->play_state_ == PS_PAUSED && this->player_) {
          this->request_player_(PlayerOperation::RESUME);
          this->save_resume_playback_(true);
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
        if (this->voice_active_) { this->voice_resume_radio_ = false; this->has_pending_media_ = false; }
        if (this->player_) {
          this->request_player_(PlayerOperation::STOP);
          this->player_running_ = false;
          this->play_state_ = PS_STOPPED;
          this->save_resume_playback_(false);
          this->announcing_ = false;
          this->announce_pending_ = false;
          if (this->pa_pin_ >= 0 && !i2s_bridge::I2SBridge::is_active())
            gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 0);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        int v = this->vol_ + 1;
        if (v > 21) v = 21;
        this->set_volume_direct(v);
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        int v = this->vol_ - 1;
        if (v < 0) v = 0;
        this->set_volume_direct(v);
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        if (!this->is_muted_) {
          this->is_muted_ = true;
          this->sw_vol_gain_ = 0;
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        if (this->is_muted_) {
          this->is_muted_ = false;
          this->sw_vol_gain_ = map_vol_q8_(this->vol_);
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
  if (this->voice_active_) {
    // Mic/TTS owns the audio path; remember the intent and resume afterwards
    this->voice_resume_radio_ = true;
    return;
  }
  const char *url = stations_[this->current_station_].url;
  ESP_LOGI(TAG, "Station: [%d] %s → %s",
           (int)this->current_station_,
           stations_[this->current_station_].name, url);

  // Write station name as initial title (ICY metadata will override)
  this->id3_artist_[0] = '\0';
  this->id3_title_[0] = '\0';
  int write_idx = 1 - this->title_read_idx_.load(std::memory_order_relaxed);
  strlcpy(this->title_bufs_[write_idx], stations_[this->current_station_].name, 128);
  this->title_read_idx_.store(write_idx, std::memory_order_release);

  // ESP-GMF detects codec from URI extension (strrchr for '.').
  // Extensionless URLs (Amperwave, Audacy, etc.) need a hint fragment.
  char uri_buf[300];
  const char *ext = strrchr(url, '.');
  bool needs_hint = !ext || strchr(ext, '/');  // no extension or '.' is in hostname
  if (needs_hint) {
    snprintf(uri_buf, sizeof(uri_buf), "%s#.aac", url);
  } else {
    strlcpy(uri_buf, url, sizeof(uri_buf));
  }

  if (this->request_player_(PlayerOperation::RUN, uri_buf)) {
    this->stream_failed_ = false;
    this->save_resume_playback_(true);
  }

  this->watchdog_stall_count_ = 0;

  // Enable PA (deferred 200ms) — only if BT bridge isn't active
  if (this->pa_pin_ >= 0 && !this->pa_pending_ &&
      !i2s_bridge::I2SBridge::is_active()) {
    this->pa_pending_ = true;
    this->pa_pending_ms_ = millis();
  }
}

void InternetRadio::next_station_() {
  int sta = (this->current_station_ + 1) % NUM_STATIONS;
  ESP_LOGI(TAG, "Station change: source=next %d -> %d (%s)",
           (int)this->current_station_, sta, stations_[sta].name);
  this->current_station_ = sta;
  int save_sta = sta;
  this->station_pref_.save(&save_sta);
  this->connect_station_();
  this->publish_station_select_();
}

void InternetRadio::prev_station_() {
  int sta = this->current_station_ > 0 ? this->current_station_ - 1 : 0;
  if (sta == this->current_station_) {
    ESP_LOGI(TAG, "Station change: source=previous already at first station");
    return;
  }
  ESP_LOGI(TAG, "Station change: source=previous %d -> %d (%s)",
           (int)this->current_station_, sta, stations_[sta].name);
  this->current_station_ = sta;
  int save_sta = sta;
  this->station_pref_.save(&save_sta);
  this->connect_station_();
  this->publish_station_select_();
}

void InternetRadio::set_station(int idx, const char *source) {
  if (idx < 0 || idx >= NUM_STATIONS || idx == this->current_station_) return;
  ESP_LOGI(TAG, "Station change: source=%s %d -> %d (%s)",
           source ? source : "unknown", (int)this->current_station_,
           idx, stations_[idx].name);
  this->current_station_ = idx;
  int save_sta = idx;
  this->station_pref_.save(&save_sta);
  this->connect_station_();
  this->publish_station_select_();
}

void InternetRadio::save_resume_playback_(bool should_resume) {
  if (this->resume_playback_ == should_resume) return;
  this->resume_playback_ = should_resume;
  int save_play = should_resume ? 1 : 0;
  this->play_pref_.save(&save_play);
  global_preferences->sync();
  ESP_LOGD(TAG, "NVS: saved resume=%d", save_play);
}

void InternetRadio::set_volume_direct(int vol) {
  if (vol < 0) vol = 0;
  if (vol > 21) vol = 21;
  this->vol_ = vol;
  this->sw_vol_gain_ = voice_logic::playback_gain(map_vol_q8_(vol), this->is_muted_, false, 0);
  ESP_LOGD(TAG, "Volume: step=%d q8=%d", vol, (int)this->sw_vol_gain_);
  this->volume = (float)vol / 21.0f;
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
  this->last_published_announcing_ = this->announcing_;
  if (this->announcing_) {
    this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
  } else switch (this->play_state_) {
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
  const char *title = this->title_bufs_[this->title_read_idx_.load(std::memory_order_acquire)];
  strlcpy(this->last_published_title_, title, sizeof(this->last_published_title_));
}

const char *InternetRadio::get_playback_phase() const {
  return playback_logic::phase(this->player_control_fault_.load(), this->announcing_, this->voice_active_,
                               int(this->play_state_), this->playback_requested_.load(),
                               this->player_command_pending_(), this->stream_failed_);
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
    global_preferences->sync();  // flush to NVS immediately
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
  int wi = 1 - this->title_read_idx_.load(std::memory_order_relaxed);
  if (strcmp(this->title_bufs_[wi], combined) == 0) return;
  strlcpy(this->title_bufs_[wi], combined, 128);
  this->title_read_idx_.store(wi, std::memory_order_release);
}

}  // namespace internet_radio
}  // namespace esphome
