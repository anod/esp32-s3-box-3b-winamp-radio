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

### Development builds

- Compile radio development sources in the existing Home Assistant ESPHome Device Builder
  on Proxmox VM 100, not on the development PC: `python3 tools/ha_build.py compile`.
- The tool syncs uncommitted YAML/components privately, uses local external components,
  and leaves the existing HA radio config and secrets unchanged.
- Keep ESPHome 2026.10.2 and native ESP-IDF 6.0.1 for the current milestone.
  Do not flash, restart HA, or change USB passthrough without user permission.
- Native audio experiments use `experiments/native-audio.yaml` and
  `python3 tools/ha_native_build.py compile`: isolated ESPHome 2026.11.0b2 in
  the same HA container, separate build/config paths, no add-on upgrade or upload.
  The probe is not feature-equivalent to the radio; beta OTA needs explicit
  permission and a verified stable rollback artifact.
- Native duplex was hardware-tested on 2026-11-09: mic capture continued through
  playback/pause/stop, and speaker output continued after mic stop. Stable radio
  was restored. Stock `speaker_source` STOP blocked the main loop for 319 ms;
  the new GMF source keeps control off the main loop. Native GMF AAC/MP3/ICY and
  independent radio/fixed voice gain passed later hardware trials (55 ms max loop);
  shared pre-gain spectrum capture passed a separate trial (68 ms max loop).
  Native wake/UI load and audible fidelity are not yet validated.
- `experiments/native-audio-bt.yaml` / `ha_native_build.py compile --bluetooth`
  is an isolated experimental I2S1 variant. Windows USB -> WSL boot logs proved
  the initial rollback was an uninitialized resampler event group: template
  switch restoration called `set_tap_enabled(false)` before speaker setup.
  Stop/finish a PCM tap only after it has actually been started, not merely
  because it is configured. No I2S driver workaround was needed.
  A short reversible routing trial passed (48 kHz internal, 44.1 kHz BT, continuous
  mic, muted FFT, BT toggles and internal-only voice). Settled windows had no tap
  drops, but transitions did. Full-load/soak and audible validation remain;
  do not treat it as Sendspin-ready. HA's Zigbee dongle is NOT the radio console.
- `experiments/native-audio-voice.yaml` adds standard wake/assistant clients on
  the shared native 16 kHz mic. Build with `--voice` (and optionally `--bluetooth`).
  Its test session scripts are not production source arbitration: GMF is paused
  locally, but future Sendspin must be ducked without pausing its group.
- Native HTTP/voice-to-HTTPS reuse exposed incorrect hardware SHA-256 digests:
  PSA returned success but disagreed with independent libsodium hashing.
  The native probe sets `CONFIG_MBEDTLS_HARDWARE_SHA=n`; retain certificate
  verification and the CA bundle. Do not mask failures with insecure TLS,
  automatic retries or higher timeouts. Stable production configuration is unchanged.
- Native voice/wake session checks and real HA Assist start/listen/stop passed
  individually. Combined readiness is still blocked: HTTP/voice -> first HTTPS
  reuse fails (hardware SHA: incorrect certificate digests; software SHA: open
  stalls, startup/control timeouts). HTTPS-first AAC/MP3 passed six cases on
  each image. Do not claim the SHA setting alone fixes reuse. Two 180-second
  wake+BT windows also dropped 148 / 69,408 tap bytes. Full display/cues, actual
  spoken wake/STT/reply and long soak are not validated. All trials restored Phase1.
- The phase-1 rollback binary and metadata are preserved independently in
  `/data/radio-native-beta/rollback/phase1/`, with an immutable source wrapper at
  `/config/esphome/esp32radio-rollback-phase1.yaml`. Verify its recorded SHA before
  `esphome upload --file`; rebuilding development firmware must not replace it.

### Component Layout

```
esp32radio.yaml                  # Main config: board, WiFi, API, OTA, components
secrets.yaml                     # WiFi/API credentials (gitignored)
components/
├── internet_radio/              # Parent namespace package
│   ├── __init__.py              # Defines internet_radio_ns
│   └── media_player/
│       ├── __init__.py          # Schema, codegen, codec deps
│       ├── internet_radio.h     # MediaPlayer subclass
│       ├── internet_radio.cpp   # ESP-GMF pipeline, ICY metadata, volume
├── i2s_bridge/
│   ├── __init__.py              # Defines i2s_bridge_ns
│   └── switch/
│       ├── __init__.py          # Schema, codegen
│       ├── i2s_bridge.h         # Switch subclass + static state
│       ├── i2s_bridge.cpp       # I2S1 TX channel management
│       └── audio_process_i2s.cpp  # Weak override (MUST be separate TU)
├── gmf_audio/                  # Shared codec allocator, HTTP/ICY and lifecycle;
│                               # beta-only native MediaSource
├── audio_gain/                 # Independent software gain; accepted radio PCM taps
├── audio_spectrum/             # Shared PCM capture + host-tested FFT
└── winamp_display/
    ├── __init__.py              # Schema, codegen, LovyanGFX + esp_lcd
    ├── winamp_display.h         # Component + I2CDevice
    ├── winamp_display.cpp       # Rendering (~200 LOC draw_frame_)
    ├── winamp_touch.cpp         # Touch via ESPHome I2C bus
```

### Core Threading Model

- **Core 0**: ESP-GMF worker thread — runs HTTP fetch, decoder, and calls our PCM output + HTTP event callbacks
- **Core 1**: ESPHome main loop — UI rendering (~15fps), touch input, WiFi, HA API, player control
- **Core 1 load**: the priority-1 main loop (~50% steady) shares Core 1 with `box3_mic` capture (priority 5, ~7%) and upstream `micro_wake_word` inference (`mww`, priority 3, FPU-pinned to Core 1, ~30%). lwIP `tcpip` is pinned to Core 0 (`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`) so fast downloads do not spill onto Core 1. Line-rate invalid streams still cause ~200–350ms main-loop gaps (shared cache/PSRAM contention slows `mww`); audio, mic capture and wake are unaffected.
- **Control flow**: Core 1 posts coalesced commands to a Core 0 control worker (priority 6). Only that worker calls blocking GMF stop/run/pause/resume. Teardown callbacks/PCM are fenced; discarded PCM yields so decoder priority 5 cannot starve control or idle tasks. Never restore blocking player calls to the main loop.
- **Callbacks on Core 0**: `pcm_output_cb_` (volume scaling, I2S write, FFT feed), `http_event_cb_` (ICY metadata), `player_event_cb_` (state changes, sample rate detection)

### Cross-Core Communication

- **Song titles**: shared HTTP/ICY metadata uses an epoch-fenced mutex mailbox;
  Core 1 transfers it to the legacy title buffers. GMF ID3 still updates those
  double buffers on Core 0, publishing `title_read_idx_` with release ordering.
  Readers use acquire ordering. New stream requests clear pending ICY metadata.
- **Volume**: `volatile int sw_vol_gain_` (Q8 fixed-point). Core 1 writes, Core 0 reads in `pcm_output_cb_`. Single-word atomic on ESP32.
- **Play state**: `volatile PlayState play_state_` updated by both cores (Core 0 via `player_event_cb_`, Core 1 via `control()`).
- **Frame counter**: `volatile uint32_t g_audio_frame_count` incremented in `pcm_output_cb_` on Core 0, read by `loop()` on Core 1 for underrun watchdog.

### FFT Spectrum Visualizer

Split across two cores for zero audio-path overhead:

- **Core 0** (`feed_fft_samples` called from `pcm_output_cb_`): Lightweight sample capture — mixes stereo to mono, fills double-buffered accumulator, flips buffer when 128 samples collected. ~microseconds per callback.
- **Core 1** (`spectrum_compute()` called from `draw_frame_`): Reads completed sample buffer, runs 128-point radix-2 FFT, aggregates into 16 logarithmic frequency bands (magnitude²). ~100μs — negligible within 55-78ms frame time.
- **Snapshot mailbox**: Core 0 fills a private 128-sample accumulator and copies
  completed blocks under a short mutex. Core 1 snapshots under that mutex, then
  releases it before FFT computation. It never races a producer reusing a buffer.
- **No esp_dsp dependency**: Self-contained FFT using only `<math.h>`. Pre-computed Hann window and twiddle factors.
- `feed_fft_samples` lives in `audio_spectrum/audio_spectrum.cpp`; both its legacy
  stereo entry point and native mono/stereo entry point remain strong symbols.

### Software Volume

- UI volume: 22 discrete steps (0–21), stored in NVS
- Mute is independent of volume in HA and touch controls. Changing volume does not
  unmute; volume zero is silent without setting mute. Radio mute uses software gain,
  leaving fixed-level internal-speaker voice replies and cues audible.
- Internal: Q8 fixed-point gain (`sw_vol_gain_`) applied in `pcm_output_cb_` to PCM samples before I2S write
- Perceptual curve is capped at unity gain to avoid clipping hot streams; the top steps approach Q8 unity (`256`) without software boost. The ES8311 analog output still provides speaker gain.
- Pre-computed lookup table `VOL_Q8[22]` — no floating-point math in audio path
- Clipping handled via `int32_t` intermediate with saturation to `INT16_MIN`/`INT16_MAX`

### ICY/Icecast Metadata

Extracts StreamTitle from Icecast/Shoutcast streams:

- **Request**: `Icy-MetaData: 1` header added in `PRE_REQUEST` callback
- **Response headers**: `icy-metaint: N` and `icy-br` read through ESP-IDF's saved response-header API
- **Stream format**: N audio bytes → 1 length byte (×16 = metadata size) → metadata text → repeat
- **Implementation**: `icy_read_()` state machine strips metadata inline, `parse_icy_metadata_()` extracts StreamTitle
- Max metadata block: 255 × 16 = 4080 bytes. Buffer is 512 bytes — excess is silently truncated (titles >~500 chars are extremely rare).

### Component Setup Priority

Components initialize in order: InternetRadio (`LATE`) → I2SBridge (`LATE - 1`) → WinampDisplay (`LATE - 2`).

### Voice Assistant (home button, "Ask Assistant" button or `okay_nabu` wake word)

- `voice_assistant` (YAML) uses `microphone: box3_mic` + `media_player: radio`. Trigger: the round GT911 home soft key below the screen (`winamp_display: on_home_button`, starts/stops the assistant; it no longer toggles play/stop) or the "Ask Assistant" HA button. Do NOT use GPIO1: it is the BOX-3 hardware mic-mute switch (mic reads 0 while muted). Boot button only cycles brightness.
- **I2S0 is full duplex**: TX (ES8311) and RX (ES7210, DIN=GPIO16) are created in ONE `i2s_new_channel` call with IDENTICAL `i2s_std_config_t` (incl. gpio_cfg). TX must stay enabled for RX clocks. `reconfig_sample_rate_` re-clocks both channels (RX disabled first if running).
- `box3_mic` (`components/box3_mic/`): capture task reads RX stereo @16k, picks `channel: left|right`, pushes mono to a stream buffer; `loop()` (Core 1) delivers to `data_callbacks_`. It logs `Mic peak: left/right` once per second — use it to pick the right channel. ES7210 comes from `audio_adc: es7210` (I2C setup only; no `i2s_audio`).
- **Voice session flow**: `voice_capture_begin()` stops the radio (without touching the NVS resume flag), clocks I2S0 to 16k, enables RX. TTS arrives as `play_media` with `announcement=true` → `announcing_`, state published as `ANNOUNCING`; FINISHED/STOPPED while announcing does NOT set `stream_failed_`. YAML `on_end` waits for `not announcing`, then `voice_session_end()` → `restore_after_voice_()` reconnects the station if it was playing. Watchdog is suspended during voice/announcement.
- **Session safety rules**: a new `voice_capture_begin()` cancels any pending restore; restore runs only when not announcing, no queued announcement, `mic_requested_` false and RX disabled. Standalone announcements (`assist_satellite.announce`, preannounce + ENQUEUE) act as mini sessions and resume the radio afterwards. `connect_station_()`, stream retry and auto-play are deferred while `voice_active_`. `reconfig_sample_rate_` and RX enable/disable are serialized by `i2s_cfg_mutex_`. YAML has a 30s `wait_until` timeout, a 300ms follow-up grace in `on_end` and a 2s `interval` safety net calling `voice_session_end()`; C++ requires the end request to be ≥500ms old before restoring. `play_media` during a session is deferred (`pending_media_`); STOP/PAUSE/TURN_OFF cancel the resume. A stalled announcement (10s without frames) is aborted. `ESP_ASP_STATE_STOPPED` does NOT clear `announcing_` (only FINISHED/ERROR or explicit Core 1 stops do). Replies use a fixed gain (`VOICE_REPLY_GAIN_Q8`), go only to the internal speaker (PA forced on), never to the BT bridge.
- Announcement format advertised: stereo MP3 @44.1k with `sample_bytes=0` (PCM path is stereo-only, resampler disabled). Do NOT advertise `sample_bytes=2` with mp3: HA's ffmpeg then fails (`libmp3lame: sample format s16 not supported`) and `tts_proxy` returns HTTP 500 → silent reply.
- **Async player control**: `esp_audio_simple_player_stop` can block seconds (HTTP teardown). All stop/start/pause/resume operations run on `radio_control`, with a one-entry latest-request mailbox and generation fencing. `voice_capture_begin()` returns -1 while stopping; RX re-clocking waits for completion. A 10s control timeout quarantines output until the worker returns; never silently continue after a timeout or kill a library task. Standalone announcements wait for the wake capture task to release RX. A 20s startup deadline requires actual I2S PCM and is stamped by the worker at launch (`playback_launched_`), never at enqueue. STOP/RUN bump `stream_epoch_`; `http_event_cb_` fences every hook against it (return -1 ⇒ GMF's reconnect fails at PRE_REQUEST, IO aborts the data bus, the pipeline stops itself), and `http_read_` polls in 200 ms slices so a dribbling server can't block teardown — do NOT "fix" stalls by raising timeouts or auto-restarting. A failed PAUSE/RESUME triggers an explicit cleanup STOP inside the same worker command. Never issue two waited GMF stops back to back (`PipelineStopTracker`): the priority-6 worker wakes on `STOP_BIT` before the priority-5 GMF thread clears its RUNNING action, so an immediate second stop (e.g. a RUN superseded during teardown) waits forever. PAUSE clears `playback_requested_`, so a paused radio is not resumed after voice/announcements. An open that fails after connecting (hook cancel at ON_REQUEST, non-200 status) gets no GMF IO close ("Failed on prepare") and the client is reused; `esp_http_client_open()` would then connect over the live transport and leak the socket plus TLS session (~22 KB PSRAM + ~7 KB internal; LWIP_MAX_SOCKETS=10). `http_event_cb_` closes the connection on a cancelled ON_REQUEST and closes any stale fd at PRE_REQUEST (counted in `http_close=` of the Runtime diag). Two GMF task races need the worker's help, never an automatic RUN retry: (a) after a failed prepare GMF fires ERROR on its own thread *before* deleting the job list, so a RUN registered in between loses its jobs and times out after 5 s — `wait_gmf_parked_()` waits (≤500 ms) until that thread is blocked again (`gmf_settle=`); (b) a run that ends by itself leaves STOP_BIT set until the next run reaches its job loop, so a stop issued while that run is still opening returns OK at once and leaves it running — `stop_pipeline_()` requires a STOPPED/FINISHED/ERROR event for a launched run and otherwise stops once more (`gmf_stop=retries,unconfirmed`). Validate with `tools/hardware_validate.py --scenario lifecycle` (slow + invalid streams) and the full-log event counters. YAML calls `display.set_voice_state(1)` immediately on trigger.
- **AAC memory**: The codec's weak `media_lib_module_*` hooks are overridden in `codec_memory.cpp` to allocate CPU-only workspaces in PSRAM; the explicit `initialize_codec_memory()` anchor is required to retain this archive member. Linked-ELF tests must confirm strong (`T`) overrides. TLS also uses PSRAM. Do not move DMA buffers there. The measured AAC workspace is 35,460 bytes; failures occurred with a 31,744-byte largest internal block even with 72KB free.
- **Playback truth**: Only actual I2S0 writes enter PLAYING. `Audio diag` has decoded and written bytes; counters use atomic exchanges. The public Playback Phase sensor exposes announcement/connecting/control-timeout states because pinned ESPHome 2026.10.2 maps ANNOUNCING to PLAYING on the native API.
- **Display during voice**: auto-dim is suppressed while `voice_state_ != IDLE` (the paused radio would otherwise look "stopped"). The reply stays up for 4s + 40ms/byte (cap 15s) via `voice_hold_`; errors expire after 4s; a new `set_voice_state` cancels the hold.
- **Audible voice cues**: `on_listening` requests a rising 120 ms chime when the assistant is ready for speech, not when the starting screen appears; `on_error` requests a falling 200 ms tone via `play_voice_cue(error)`. Cues do not wait for HTTP/player teardown. PCM is synthesized on a short-lived task, at a fixed level, on the internal speaker only (also with BT enabled). The task shares `i2s_cfg_mutex_` with re-clocking and PCM writes; internal player output is ducked while the cue drains. Radio restore waits for pending/active cues. No media files or network requests are needed.
- **Full-screen voice view**: while `voice_state_ != IDLE`, `draw_frame_()` draws `draw_voice_view_()` into the SAME PSRAM canvas (no extra sprites, so switching is the next frame). With no text yet, a big mic (1.5x) is centered with the state label and, while LISTENING, a live mic meter (`voice_logic::g_mic_peak` envelope written by the Box3Mic task, `level_pct()` log scale). Exactly-zero samples for 2.5s show "MIC MUTED?" (GPIO1 hardware mute). Once text arrives the icon shrinks to the top (state label top-left), recognized text (dim) + reply (white) below. Per-state animation: rings / spinner / equalizer / error slash. The bottom strip (ticker sprite) continues the voice background and frame (drawn directly on the panel).
- **Text layout** (`text_layout.h`, host-tested): UTF-8 word wrap + simplified bidi (Hebrew runs reversed, RTL paragraph right-aligned, brackets mirrored). Wrapping/reordering runs only when text changes (`voice_layout_dirty_`), not per frame. Max 2 heard + 8 reply lines (small font), truncated with "...". Text buffers are 256/1024 BYTES (UTF-8, 2 bytes/char for Cyrillic/Hebrew).
- **Debug screenshots**: API actions `preview_voice(state, heard, reply, mic_peak)` and `screenshot` (freezes the frame, streams the canvas as `SHOT <offset> <base64>` log lines, RGB565 big-endian, 320x218). Capture with `esphome logs` and decode with PIL.
- Wake word: `micro_wake_word` (`okay_nabu`, "Wake Word" switch) listens through a second `box3_mic` with `wake_mode: true` (radio-rate RX resampled to 16 kHz) while the radio plays; it is stopped for button sessions and restarted after voice/announcements (validator checks `restores_with_wake`).
- Verified on hardware: button trigger, Russian STT, on-screen text, spoken reply, radio resume, wake capture restart. Untested: actual spoken wake-word detection (needs a human speaker).
