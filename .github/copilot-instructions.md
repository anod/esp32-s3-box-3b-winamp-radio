# Copilot Instructions — ESP32-S3-BOX-3 Internet Radio

> **⚠️ Before every commit, re-read the Code Review Checklist and ESPHome Pitfalls sections below.** These encode hard-won debugging lessons. Skipping them risks reintroducing bugs that took hours to diagnose.

## Project Overview

ESPHome-based internet radio for the ESP32-S3-BOX-3. Uses ESP-IDF framework with ESP-GMF audio pipeline. Custom external components in `components/` handle audio streaming (HTTP→decoder→PCM→I2S), BT speaker bridge, and Winamp 2 display. ESPHome provides OTA, native HA API, and WiFi management.

- **ESPHome firmware** (repo root): `esp32radio.yaml` config + `components/` custom external components for audio streaming, BT speaker bridge, and Winamp 2 display.
- **BT bridge firmware** (`bt-bridge/`): Separate PlatformIO project for ESP32-WROOM-32D (I2S→A2DP bridge, builds independently).
- **Legacy firmware** (`archive/platformio/`): Original standalone Arduino firmware with MQTT. Kept as reference only.

## Hardware

- **Board**: ESP32-S3-BOX-3 (ESP32-S3, 16MB flash, 8MB OPI PSRAM)
- **Display**: ILI9342C 320×240 SPI (managed by LovyanGFX autodetect)
- **Audio**: ES8311 I2C codec → I2S0 → built-in speaker (PA on GPIO 46)
- **Touch**: GT911 capacitive controller (shared I2C bus with codec). The red circle "home button" below the screen is a GT911 soft key — NOT a coordinate touch.
- **Home button**: GT911 soft key, read via I2C register 0x814E bit 4. Must be read BEFORE LovyanGFX `getTouch()` which clears the register.
- **I2S bridge output**: I2S1 TX on GPIO 10/14/11 → wired to ESP32-WROOM-32D for Bluetooth A2DP
- **Critical pin note**: BOX-3 uses I2S_LRCK=GPIO45, LCD_BL=GPIO47. BOX v1 has these swapped — do NOT mix them.

## ESPHome Architecture

### Component Layout

```
esp32radio.yaml                  # Main config: board, WiFi, API, OTA, components
secrets.yaml                     # WiFi/API credentials (gitignored)
components/
├── internet_radio/              # Parent namespace package
│   ├── __init__.py              # Defines internet_radio_ns
│   └── media_player/
│       ├── __init__.py          # Schema, codegen, IDF component deps
│       ├── internet_radio.h     # MediaPlayer subclass
│       ├── internet_radio.cpp   # ESP-GMF pipeline, ICY metadata, volume
│       └── patch_gmf_http.py    # Pre-build: patches GMF for ICY header capture
├── i2s_bridge/
│   ├── __init__.py              # Defines i2s_bridge_ns
│   └── switch/
│       ├── __init__.py          # Schema, codegen
│       ├── i2s_bridge.h         # Switch subclass + static state
│       ├── i2s_bridge.cpp       # I2S1 TX channel management
│       └── audio_process_i2s.cpp  # Weak override (MUST be separate TU)
└── winamp_display/
    ├── __init__.py              # Schema, codegen, LovyanGFX + esp_lcd
    ├── winamp_display.h         # Component + I2CDevice
    ├── winamp_display.cpp       # Rendering (~200 LOC draw_frame_)
    ├── winamp_touch.cpp         # Touch via ESPHome I2C bus
    └── spectrum.cpp             # FFT visualizer (separate TU)
```

### Core Threading Model

- **Core 0**: ESP-GMF worker thread — runs HTTP fetch, decoder, and calls our PCM output + HTTP event callbacks
- **Core 1**: ESPHome main loop — UI rendering (~15fps), touch input, WiFi, HA API, player control
- **Control flow**: Core 1 calls `esp_audio_simple_player_play/stop/pause/resume()` directly — ESP-GMF API is thread-safe
- **Callbacks on Core 0**: `pcm_output_cb_` (volume scaling, I2S write, FFT feed), `http_event_cb_` (ICY metadata), `player_event_cb_` (state changes, sample rate detection)

### Cross-Core Communication

- **Song title double-buffer**: `std::atomic<int> title_read_idx_` with acquire/release ordering. Core 0 writes to `title_bufs_[1 - idx]`, then `store(wi, release)`. Core 1 reads `title_bufs_[idx.load(acquire)]`. No mutex needed.
- **Volume**: `volatile int sw_vol_gain_` (Q8 fixed-point). Core 1 writes, Core 0 reads in `pcm_output_cb_`. Single-word atomic on ESP32.
- **Play state**: `volatile PlayState play_state_` updated by both cores (Core 0 via `player_event_cb_`, Core 1 via `control()`).
- **Frame counter**: `volatile uint32_t g_audio_frame_count` incremented in `pcm_output_cb_` on Core 0, read by `loop()` on Core 1 for underrun watchdog.

### FFT Spectrum Visualizer

Split across two cores for zero audio-path overhead:

- **Core 0** (`feed_fft_samples` called from `pcm_output_cb_`): Lightweight sample capture — mixes stereo to mono, fills double-buffered accumulator, flips buffer when 128 samples collected. ~microseconds per callback.
- **Core 1** (`spectrum_compute()` called from `draw_frame_`): Reads completed sample buffer, runs 128-point radix-2 FFT, aggregates into 16 logarithmic frequency bands (magnitude²). ~100μs — negligible within 55-78ms frame time.
- **Double buffer**: `sample_bufs[2][128]` with volatile `write_buf_` index. Core 0 writes to one buffer, Core 1 reads the other. `sample_ready` flag set last as release signal.
- **No esp_dsp dependency**: Self-contained FFT using only `<math.h>`. Pre-computed Hann window and twiddle factors.
- `feed_fft_samples` and `audio_process_i2s` MUST live in their own `.cpp` files — weak-symbol overrides require separate translation units.

### Software Volume

- UI volume: 22 discrete steps (0–21), stored in NVS
- Internal: Q8 fixed-point gain (`sw_vol_gain_`) applied in `pcm_output_cb_` to PCM samples before I2S write
- Perceptual curve is capped at unity gain to avoid clipping hot streams; the top steps approach Q8 unity (`256`) without software boost. The ES8311 analog output still provides speaker gain.
- Pre-computed lookup table `VOL_Q8[22]` — no floating-point math in audio path
- Clipping handled via `int32_t` intermediate with saturation to `INT16_MIN`/`INT16_MAX`

### ICY/Icecast Metadata

Extracts StreamTitle from Icecast/Shoutcast streams:

- **Request**: `Icy-MetaData: 1` header added in `PRE_REQUEST` callback
- **Response header**: `icy-metaint: N` captured by patched GMF HTTP handler (stored in `volatile int g_icy_metaint`)
- **Stream format**: N audio bytes → 1 length byte (×16 = metadata size) → metadata text → repeat
- **Implementation**: `icy_read_()` state machine strips metadata inline, `parse_icy_metadata_()` extracts StreamTitle
- **Patch script**: `patch_gmf_http.py` — required because `esp_http_client` has no public API to read response headers (they're dispatched via event callback then freed immediately)
- Max metadata block: 255 × 16 = 4080 bytes. Buffer is 512 bytes — excess is silently truncated (titles >~500 chars are extremely rare).

### Component Setup Priority

Components initialize in order: InternetRadio (`LATE`) → I2SBridge (`LATE - 1`) → WinampDisplay (`LATE - 2`).

### Voice Assistant (button-triggered, no wake word)

- `voice_assistant` (YAML) uses `microphone: box3_mic` + `media_player: radio`. Trigger: the round GT911 home soft key below the screen (`winamp_display: on_home_button`, starts/stops the assistant; it no longer toggles play/stop) or the "Ask Assistant" HA button. Do NOT use GPIO1: it is the BOX-3 hardware mic-mute switch (mic reads 0 while muted). Boot button only cycles brightness.
- **I2S0 is full duplex**: TX (ES8311) and RX (ES7210, DIN=GPIO16) are created in ONE `i2s_new_channel` call with IDENTICAL `i2s_std_config_t` (incl. gpio_cfg). TX must stay enabled for RX clocks. `reconfig_sample_rate_` re-clocks both channels (RX disabled first if running).
- `box3_mic` (`components/box3_mic/`): capture task reads RX stereo @16k, picks `channel: left|right`, pushes mono to a stream buffer; `loop()` (Core 1) delivers to `data_callbacks_`. It logs `Mic peak: left/right` once per second — use it to pick the right channel. ES7210 comes from `audio_adc: es7210` (I2C setup only; no `i2s_audio`).
- **Voice session flow**: `voice_capture_begin()` stops the radio (without touching the NVS resume flag), clocks I2S0 to 16k, enables RX. TTS arrives as `play_media` with `announcement=true` → `announcing_`, state published as `ANNOUNCING`; FINISHED/STOPPED while announcing does NOT set `stream_failed_`. YAML `on_end` waits for `not announcing`, then `voice_session_end()` → `restore_after_voice_()` reconnects the station if it was playing. Watchdog is suspended during voice/announcement.
- **Session safety rules**: a new `voice_capture_begin()` cancels any pending restore; restore runs only when not announcing, no queued announcement, `mic_requested_` false and RX disabled. Standalone announcements (`assist_satellite.announce`, preannounce + ENQUEUE) act as mini sessions and resume the radio afterwards. `connect_station_()`, stream retry and auto-play are deferred while `voice_active_`. `reconfig_sample_rate_` and RX enable/disable are serialized by `i2s_cfg_mutex_`. YAML has a 30s `wait_until` timeout, a 300ms follow-up grace in `on_end` and a 2s `interval` safety net calling `voice_session_end()`; C++ requires the end request to be ≥500ms old before restoring. `play_media` during a session is deferred (`pending_media_`); STOP/PAUSE/TURN_OFF cancel the resume. A stalled announcement (10s without frames) is aborted. `ESP_ASP_STATE_STOPPED` does NOT clear `announcing_` (only FINISHED/ERROR or explicit Core 1 stops do). Replies use a fixed gain (`VOICE_REPLY_GAIN_Q8`), go only to the internal speaker (PA forced on), never to the BT bridge.
- Announcement format advertised: stereo MP3 @44.1k with `sample_bytes=0` (PCM path is stereo-only, resampler disabled). Do NOT advertise `sample_bytes=2` with mp3: HA's ffmpeg then fails (`libmp3lame: sample format s16 not supported`) and `tts_proxy` returns HTTP 500 → silent reply.
- **Async player stop**: `esp_audio_simple_player_stop` can block ~3s (HTTP teardown). `voice_capture_begin()` pauses the player (silences TX) and stops it on a `va_stop` task (`stop_state_`); it returns 1 ok, 0 fail, -1 = still stopping (Box3Mic retries next loop). `play_uri_`/`connect_station_` call `wait_stop_done_()` first. YAML calls `display.set_voice_state(1)` on trigger so the listening screen shows in ~0.2s.
- **Display during voice**: auto-dim is suppressed while `voice_state_ != IDLE` (the paused radio would otherwise look "stopped"). The reply stays up for 4s + 40ms/byte (cap 15s) via `voice_hold_`; errors expire after 4s; a new `set_voice_state` cancels the hold.
- **Full-screen voice view**: while `voice_state_ != IDLE`, `draw_frame_()` draws `draw_voice_view_()` into the SAME PSRAM canvas (no extra sprites, so switching is the next frame). With no text yet, a big mic (1.5x) is centered with the state label and, while LISTENING, a live mic meter (`voice_logic::g_mic_peak` envelope written by the Box3Mic task, `level_pct()` log scale). Exactly-zero samples for 2.5s show "MIC MUTED?" (GPIO1 hardware mute). Once text arrives the icon shrinks to the top (state label top-left), recognized text (dim) + reply (white) below. Per-state animation: rings / spinner / equalizer / error slash. The bottom strip (ticker sprite) continues the voice background and frame (drawn directly on the panel).
- **Text layout** (`text_layout.h`, host-tested): UTF-8 word wrap + simplified bidi (Hebrew runs reversed, RTL paragraph right-aligned, brackets mirrored). Wrapping/reordering runs only when text changes (`voice_layout_dirty_`), not per frame. Max 2 heard + 8 reply lines (small font), truncated with "...". Text buffers are 256/1024 BYTES (UTF-8, 2 bytes/char for Cyrillic/Hebrew).
- **Debug screenshots**: API actions `preview_voice(state, heard, reply, mic_peak)` and `screenshot` (freezes the frame, streams the canvas as `SHOT <offset> <base64>` log lines, RGB565 big-endian, 320x218). Capture with `esphome logs` and decode with PIL.
- Verified on hardware: button trigger, Russian STT, on-screen text, spoken reply, radio resume. Untested: wake word.
