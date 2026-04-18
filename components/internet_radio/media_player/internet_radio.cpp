// ============================================================
// internet_radio — ESPHome MediaPlayer implementation
// ESP-GMF pipeline: HTTP → decoder → PCM callback → I2S0
// ============================================================

#include "internet_radio.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include "../../i2s_bridge/switch/i2s_bridge.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "driver/i2c_master.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

// ─── ICY metaint: captured by patched GMF HTTP event handler ───
// The pre-build script (patch_gmf_http.py) adds icy-metaint header
// extraction to the GMF HTTP IO internal event handler. It stores
// the value in this global, which we read on first ON_RESPONSE.
extern "C" volatile int g_icy_metaint;

// FFT sample feed (defined in spectrum.cpp)
extern void feed_fft_samples(const uint8_t *data, int size);

// Audio frame counter — incremented in pcm_output_cb_ on Core 0.
// Read by loop() on Core 1 for underrun detection.
volatile uint32_t g_audio_frame_count = 0;

namespace esphome {
namespace internet_radio {

static const char *const TAG = "internet_radio";

// Software volume: UI step (0–21) → Q8 fixed-point gain.
// Perceptual dB curve: -42 dB to +6 dB in 21 steps (2.29 dB/step).
// gain = 10^(dB/20) * 256, where dB = -42 + step * (48/21).
// Step 18 ≈ unity (0 dB), steps 19-21 provide +2/+4/+6 dB boost.
// Clipping is handled in pcm_output_cb_ (int32_t→int16_t saturation).
static const uint16_t VOL_Q8[22] = {
    0,                                          // 0: mute
    3,   3,   5,   6,   8,  10,  13,  17,      // 1–8:  ~-40 to -24 dB
    22,  28,  37,  48,  62,  81, 106, 138,      // 9–16: ~-21 to -5 dB
    179, 231, 301, 392, 512                     // 17–21: ~-3 to +6 dB
};

int InternetRadio::map_vol_q8_(int step) {
  if (step <= 0) return 0;
  if (step >= 21) return 512;
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

  // Set software volume from NVS BEFORE hardware init (codec may fail on cold boot)
  this->sw_vol_gain_ = map_vol_q8_(this->vol_);

  // Initialize I2S and player — ES8311 codec deferred to loop() (I2C/MCLK not ready)
  this->init_i2s0_();
  this->init_player_();

  // Set initial HA state
  this->volume = (float)this->vol_ / 21.0f;
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
}

// ─── Loop (Core 1 — ESPHome main loop) ───────────────────

void InternetRadio::loop() {
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

  // Retry on stream failure (with debounce to avoid rapid-fire retries)
  if (this->stream_failed_ && this->wifi_connected_) {
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
          !this->stream_failed_) {
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
  // Skip if BT bridge is active — bridge mutes internal speaker
  if (this->pa_pending_ && (millis() - this->pa_pending_ms_ >= 200)) {
    this->pa_pending_ = false;
    if (!i2s_bridge::I2SBridge::is_active()) {
      gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 1);
    }
  }

  // Debounced NVS volume save
  this->flush_vol_if_dirty_();

  // Publish state changes to HA (reads from stable title buffer)
  const char *title = this->title_bufs_[this->title_read_idx_.load(std::memory_order_acquire)];
  bool title_changed = strcmp(title, this->last_published_title_) != 0;
  if (this->play_state_ != this->last_published_state_ || title_changed) {
    this->update_ha_state_();
  }
  if (title_changed && this->now_playing_sensor_) {
    this->now_playing_sensor_->publish_state(title);
  }
}

// ─── Audio init (Core 1, called from setup) ──────────────

void InternetRadio::init_i2s0_() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &this->i2s_tx_, nullptr));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
          I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = (gpio_num_t)this->mclk_pin_,
          .bclk = (gpio_num_t)this->bclk_pin_,
          .ws = (gpio_num_t)this->lrclk_pin_,
          .dout = (gpio_num_t)this->dout_pin_,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
      },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(this->i2s_tx_, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(this->i2s_tx_));

  this->current_sample_rate_ = 44100;
  ESP_LOGI(TAG, "I2S0: BCLK=%d LRCK=%d DOUT=%d MCLK=%d (44100Hz)",
           this->bclk_pin_, this->lrclk_pin_, this->dout_pin_, this->mclk_pin_);
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
  this->sw_vol_gain_ = map_vol_q8_(this->vol_);
  ESP_LOGI(TAG, "ES8311: vol_step=%d q8=%d", (int)this->vol_, (int)this->sw_vol_gain_);

  ESP_LOGI(TAG, "ES8311 initialized (%luHz stereo 16-bit)",
           (unsigned long)this->current_sample_rate_);
}

void InternetRadio::init_player_() {
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

  ESP_LOGI(TAG, "ESP-GMF player created (Core 0, 8KB stack)");
}

void InternetRadio::init_http_io_() {
  http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
  http_cfg.dir = ESP_GMF_IO_DIR_READER;
  http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  http_cfg.event_handle = http_event_cb_;
  http_cfg.user_data = this;

  esp_gmf_err_t ret = esp_gmf_io_http_init(&http_cfg, &this->http_io_);
  if (ret != ESP_GMF_ERR_OK) {
    ESP_LOGE(TAG, "Failed to create HTTP IO: %d", ret);
    return;
  }
  esp_audio_simple_player_register_io(this->player_, this->http_io_);
  ESP_LOGI(TAG, "HTTP IO registered (HTTPS cert bundle enabled)");
}

// ─── HTTP event callback (Core 0) — ICY metadata extraction ──

// Extract StreamTitle from ICY metadata block, publish to title double-buffer.
void InternetRadio::parse_icy_metadata_() {
  icy_meta_buf_[icy_meta_idx_] = '\0';
  if (icy_meta_idx_ == 0) return;

  const char *st = strstr(icy_meta_buf_, "StreamTitle='");
  if (!st) return;
  st += 13;  // skip "StreamTitle='"
  const char *end = strstr(st, "';");
  int len = end ? (int)(end - st) : (int)strlen(st);
  if (len <= 0 || len >= 128) return;

  // Write to double-buffer (Core 0 → Core 1 via acquire/release)
  int wi = 1 - title_read_idx_.load(std::memory_order_relaxed);
  if (strncmp(title_bufs_[wi], st, len) == 0 && title_bufs_[wi][len] == '\0') return;
  memcpy(title_bufs_[wi], st, len);
  title_bufs_[wi][len] = '\0';
  title_read_idx_.store(wi, std::memory_order_release);
  ESP_LOGI(TAG, "ICY: %s", title_bufs_[wi]);
}

// Read from HTTP stream, stripping interleaved ICY metadata.
// Returns bytes of clean audio written to out, 0 on EOF, -1 on error.
int InternetRadio::icy_read_(esp_http_client_handle_t client, char *out, int out_len) {
  int out_pos = 0;
  int last_rd = 0;

  while (out_pos < out_len) {
    if (icy_meta_remaining_ > 0) {
      // Inside metadata block — consume into icy_meta_buf_
      char tmp[256];
      int to_read = icy_meta_remaining_;
      if (to_read > (int)sizeof(tmp)) to_read = (int)sizeof(tmp);
      last_rd = esp_http_client_read(client, tmp, to_read);
      if (last_rd <= 0) break;
      int space = (int)sizeof(icy_meta_buf_) - 1 - icy_meta_idx_;
      if (space > 0) {
        int n = last_rd < space ? last_rd : space;
        memcpy(icy_meta_buf_ + icy_meta_idx_, tmp, n);
        icy_meta_idx_ += n;
      }
      icy_meta_remaining_ -= last_rd;
      if (icy_meta_remaining_ <= 0) {
        parse_icy_metadata_();
        icy_meta_idx_ = 0;
        icy_remaining_ = icy_metaint_;
      }
      continue;
    }

    if (icy_remaining_ == 0) {
      // Read 1-byte metadata length prefix
      char len_byte = 0;
      last_rd = esp_http_client_read(client, &len_byte, 1);
      if (last_rd <= 0) break;
      int meta_len = (uint8_t)len_byte * 16;
      if (meta_len > 0) {
        icy_meta_remaining_ = meta_len;
        icy_meta_idx_ = 0;
      } else {
        icy_remaining_ = icy_metaint_;
      }
      continue;
    }

    // Read audio data up to next metadata boundary
    int want = out_len - out_pos;
    if (icy_remaining_ < want) want = icy_remaining_;
    last_rd = esp_http_client_read(client, out + out_pos, want);
    if (last_rd <= 0) break;
    out_pos += last_rd;
    icy_remaining_ -= last_rd;
  }

  if (out_pos > 0) return out_pos;
  return last_rd < 0 ? -1 : 0;  // propagate EOF vs error
}

int InternetRadio::http_event_cb_(http_stream_event_msg_t *msg) {
  auto *self = static_cast<InternetRadio *>(msg->user_data);

  if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
    // Request ICY metadata from Icecast/Shoutcast servers
    auto client = static_cast<esp_http_client_handle_t>(msg->http_client);
    esp_http_client_set_header(client, "Icy-MetaData", "1");
    // Reset ICY state for new connection
    g_icy_metaint = 0;
    self->icy_metaint_ = 0;
    self->icy_remaining_ = 0;
    self->icy_meta_remaining_ = 0;
    self->icy_meta_idx_ = 0;
    self->icy_header_checked_ = false;
    return 0;
  }

  if (msg->event_id == HTTP_STREAM_ON_RESPONSE) {
    auto client = static_cast<esp_http_client_handle_t>(msg->http_client);

    // On first response, check for icy-metaint header
    if (!self->icy_header_checked_) {
      self->icy_header_checked_ = true;
      int metaint = g_icy_metaint;
      if (metaint > 0) {
        self->icy_metaint_ = metaint;
        self->icy_remaining_ = metaint;
        ESP_LOGI(TAG, "ICY metaint: %d bytes", metaint);
      } else {
        ESP_LOGD(TAG, "No ICY metadata in response");
      }
    }

    if (self->icy_metaint_ <= 0) return 0;  // no ICY — let library read normally
    return self->icy_read_(client, static_cast<char *>(msg->buffer), msg->buffer_len);
  }

  return 0;
}

// ─── PCM output callback (Core 0, ESP-GMF worker thread) ─

int InternetRadio::pcm_output_cb_(uint8_t *data, int size, void *ctx) {
  auto *self = static_cast<InternetRadio *>(ctx);
  size_t written = 0;

  // 1. Feed FFT BEFORE volume scaling — visualizer needs full-amplitude PCM
  feed_fft_samples(data, size);

  // 2. Software volume: scale 16-bit PCM samples using Q8 fixed-point gain
  int gain = self->sw_vol_gain_;
  if (gain != 256) {
    int16_t *samples = reinterpret_cast<int16_t *>(data);
    int num_samples = size / 2;
    for (int i = 0; i < num_samples; i++) {
      int32_t s = (int32_t)samples[i] * gain >> 8;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      samples[i] = (int16_t)s;
    }
  }

  // 3. Write to ES8311 speaker (bounded timeout to avoid hanging stop/reconnect)
  esp_err_t err = i2s_channel_write(self->i2s_tx_, data, size, &written,
                                    pdMS_TO_TICKS(100));
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
    return -1;
  }

  // 4. Write to I2S bridge (BT speaker) if active
  // Snapshot handle to avoid race with Core 1 teardown (deinit_i2s_ nulls tx_handle_)
  i2s_chan_handle_t bridge_tx = i2s_bridge::I2SBridge::get_tx_handle();
  if (i2s_bridge::I2SBridge::is_active() && bridge_tx) {
    size_t bw = 0;
    i2s_channel_write(bridge_tx, data, size, &bw, pdMS_TO_TICKS(10));
  }

  // 5. Watchdog frame count
  g_audio_frame_count++;

  return (int)written;
}

// ─── Player event callback (Core 0, ESP-GMF worker thread)

int InternetRadio::player_event_cb_(esp_asp_event_pkt_t *pkt, void *ctx) {
  auto *self = static_cast<InternetRadio *>(ctx);

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
    }

    self->bitrate_ = info->bitrate;
    // Update bridge sample rate for WROOM-32D resampler
    i2s_bridge::I2SBridge::bridge_sample_rate = info->sample_rate;
  }

  if (pkt->type == ESP_ASP_EVENT_TYPE_STATE) {
    esp_asp_state_t state = *(esp_asp_state_t *)pkt->payload;
    ESP_LOGI(TAG, "Player state: %d", (int)state);
    switch (state) {
      case ESP_ASP_STATE_RUNNING:
        self->play_state_ = PS_PLAYING;
        self->player_running_ = true;
        // Enable PA (deferred) — only if BT bridge isn't active
        if (self->pa_pin_ >= 0 && !self->pa_pending_ &&
            !i2s_bridge::I2SBridge::is_active()) {
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
        break;
      case ESP_ASP_STATE_FINISHED:
      case ESP_ASP_STATE_ERROR:
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

  i2s_channel_disable(this->i2s_tx_);

  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(new_rate);
  clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(this->i2s_tx_, &clk));
  ESP_ERROR_CHECK(i2s_channel_enable(this->i2s_tx_));

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
    this->sw_vol_gain_ = map_vol_q8_(this->vol_);
    this->volume = v;
    this->is_muted_ = (this->vol_ == 0);
    this->mark_vol_dirty_();
    ESP_LOGD(TAG, "Volume SET: step=%d q8=%d from=%.3f", (int)this->vol_, (int)this->sw_vol_gain_, v);
  }

  if (call.get_media_url().has_value()) {
    const std::string &url = *call.get_media_url();
    ESP_LOGI(TAG, "Play URL: %s", url.c_str());
    this->id3_artist_[0] = '\0';
    this->id3_title_[0] = '\0';
    int write_idx = 1 - this->title_read_idx_.load(std::memory_order_relaxed);
    strlcpy(this->title_bufs_[write_idx], "Connecting...", 128);
    this->title_read_idx_.store(write_idx, std::memory_order_release);
    // Play the URL directly via ESP-GMF
    if (this->player_) {
      if (this->player_running_) {
        esp_audio_simple_player_stop(this->player_);
        this->player_running_ = false;
      }
      // Add codec hint for extensionless URLs
      char uri_buf[300];
      const char *ext = strrchr(url.c_str(), '.');
      bool needs_hint = !ext || strchr(ext, '/');
      if (needs_hint) {
        snprintf(uri_buf, sizeof(uri_buf), "%s#.aac", url.c_str());
      } else {
        strlcpy(uri_buf, url.c_str(), sizeof(uri_buf));
      }
      esp_gmf_err_t ret = esp_audio_simple_player_run(this->player_, uri_buf, nullptr);
      if (ret == ESP_GMF_ERR_OK) {
        this->player_running_ = true;
        this->play_state_ = PS_PLAYING;
      } else {
        ESP_LOGE(TAG, "Failed to start player: %d", ret);
        this->play_state_ = PS_STOPPED;
      }
    }
    this->watchdog_stall_count_ = 0;
  }

  if (call.get_command().has_value()) {
    ESP_LOGI(TAG, "Command received: %d", (int)*call.get_command());
    switch (*call.get_command()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        if (this->play_state_ == PS_PAUSED && this->player_) {
          esp_audio_simple_player_resume(this->player_);
        } else if (this->play_state_ == PS_STOPPED) {
          this->connect_station_();
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
        if (this->play_state_ == PS_PLAYING && this->player_) {
          esp_audio_simple_player_pause(this->player_);
          this->play_state_ = PS_PAUSED;
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        if (this->play_state_ != PS_STOPPED && this->player_) {
          esp_audio_simple_player_stop(this->player_);
          this->player_running_ = false;
          this->play_state_ = PS_STOPPED;
          if (this->pa_pin_ >= 0 && !i2s_bridge::I2SBridge::is_active())
            gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 0);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (this->play_state_ == PS_PLAYING && this->player_) {
          esp_audio_simple_player_pause(this->player_);
          this->play_state_ = PS_PAUSED;
        } else if (this->play_state_ == PS_PAUSED && this->player_) {
          esp_audio_simple_player_resume(this->player_);
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
        if (this->play_state_ != PS_STOPPED && this->player_) {
          esp_audio_simple_player_stop(this->player_);
          this->player_running_ = false;
          this->play_state_ = PS_STOPPED;
          if (this->pa_pin_ >= 0 && !i2s_bridge::I2SBridge::is_active())
            gpio_set_level(static_cast<gpio_num_t>(this->pa_pin_), 0);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        int v = this->vol_ + 1;
        if (v > 21) v = 21;
        this->vol_ = v;
        this->sw_vol_gain_ = map_vol_q8_(v);
        this->volume = (float)v / 21.0f;
        this->mark_vol_dirty_();
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        int v = this->vol_ - 1;
        if (v < 0) v = 0;
        this->vol_ = v;
        this->sw_vol_gain_ = map_vol_q8_(v);
        this->volume = (float)v / 21.0f;
        this->mark_vol_dirty_();
        break;
      }

      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        if (!this->is_muted_) {
          this->is_muted_ = true;
          this->sw_vol_gain_ = 0;
          if (this->codec_if_)
            this->codec_if_->mute(this->codec_if_, true);
        }
        break;

      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        if (this->is_muted_) {
          this->is_muted_ = false;
          this->sw_vol_gain_ = map_vol_q8_(this->vol_);
          if (this->codec_if_)
            this->codec_if_->mute(this->codec_if_, false);
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

  // Start playback via ESP-GMF pipeline
  if (this->player_) {
    if (this->player_running_) {
      esp_audio_simple_player_stop(this->player_);
      this->player_running_ = false;
    }
    esp_gmf_err_t ret = esp_audio_simple_player_run(this->player_, uri_buf, nullptr);
    if (ret == ESP_GMF_ERR_OK) {
      this->player_running_ = true;
      this->play_state_ = PS_PLAYING;
      this->stream_failed_ = false;
    } else {
      ESP_LOGE(TAG, "Failed to start player: %d", ret);
      this->play_state_ = PS_STOPPED;
      this->stream_failed_ = true;
    }
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
  this->sw_vol_gain_ = map_vol_q8_(vol);
  ESP_LOGD(TAG, "Volume: step=%d q8=%d", vol, (int)this->sw_vol_gain_);
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
  const char *title = this->title_bufs_[this->title_read_idx_.load(std::memory_order_acquire)];
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
