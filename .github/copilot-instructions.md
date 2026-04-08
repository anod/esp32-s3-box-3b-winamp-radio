# Copilot Instructions — ESP32-S3-BOX-3 Internet Radio

> **⚠️ Before every commit, re-read the Code Review Checklist and ESPHome Pitfalls sections below.** These encode hard-won debugging lessons. Skipping them risks reintroducing bugs that took hours to diagnose.

## Project Overview

Dual-implementation internet radio for the ESP32-S3-BOX-3. The **active implementation** lives in `esphome/` — custom external components running on the ESPHome platform. The legacy PlatformIO firmware in `src/` is kept as reference but is no longer the primary target.

- **ESPHome firmware** (`esphome/`): Custom external components for audio streaming, BT speaker bridge, and Winamp 2 display. Gains OTA, native HA API, WiFi management from ESPHome.
- **PlatformIO firmware** (`src/`): Original standalone Arduino firmware with MQTT integration. Kept as reference for audio/UI behavior.
- **BT bridge firmware** (`bt-bridge/`): Separate ESP32-WROOM-32D project (unchanged, builds independently).

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
esphome/
├── esp32radio.yaml              # Main config: board, WiFi, API, OTA, components
├── secrets.yaml                 # WiFi/API credentials (gitignored)
└── components/
    ├── internet_radio/          # Parent namespace package
    │   ├── __init__.py          # Defines internet_radio_ns
    │   └── media_player/
    │       ├── __init__.py      # Schema, codegen, IDF workarounds
    │       ├── internet_radio.h # MediaPlayer subclass
    │       ├── internet_radio.cpp
    │       └── stubs/esp_dsp.h  # Stub for unused Audio.h dependency
    ├── i2s_bridge/
    │   ├── __init__.py          # Defines i2s_bridge_ns
    │   └── switch/
    │       ├── __init__.py      # Schema, codegen
    │       ├── i2s_bridge.h     # Switch subclass + static state
    │       ├── i2s_bridge.cpp   # I2S1 TX channel management
    │       └── audio_process_i2s.cpp  # Weak override (MUST be separate TU)
    └── winamp_display/
        ├── __init__.py          # Schema, codegen, LovyanGFX + esp_lcd
        ├── winamp_display.h     # Component + I2CDevice
        ├── winamp_display.cpp   # Rendering (~200 LOC draw_frame_)
        └── winamp_touch.cpp     # Touch via ESPHome I2C bus
```

### Core Threading Model

- **Core 0**: Dedicated FreeRTOS audio task — calls `audio.loop()` continuously, consumes pending flags
- **Core 1**: ESPHome main loop — UI rendering (~15fps), touch input, WiFi, HA API
- **Cross-core rule**: Audio-library methods (`connecttohost`, `pauseResume`, `stopSong`) must ONLY be called from Core 0. Core 1 sets volatile flags that the audio task consumes.
- `audio.setVolume()` is the ONLY method safe to call cross-core.

### Cross-Core Communication Patterns

```
Core 1 (ESPHome loop)              Core 0 (audio_task)
─────────────────────              ────────────────────
strlcpy(pending_url_, url)    →    audio.connecttohost(pending_url_)
pending_connect_ = true

pending_pause_ = true          →    audio.pauseResume()
pending_stop_ = true           →    audio.stopSong()
audio.setVolume(v)             →    (direct call OK)
```

**Song title double-buffer**: Core 0 writes to `title_bufs_[1 - title_read_idx_]`, then flips `title_read_idx_`. Core 1 always reads `title_bufs_[title_read_idx_]`. No mutex needed — atomic index swap ensures Core 1 never reads a partially-written buffer.

**Station URL**: Copied to `pending_url_[256]` BEFORE setting `pending_connect_ = true`. Eliminates ordering race where Core 0 could read a stale `current_station_` index.

### Component Setup Priority

Components initialize in order: InternetRadio (`LATE`) → I2SBridge (`LATE - 1`) → WinampDisplay (`LATE - 2`).

### HA Integration

- ESPHome native API replaces MQTT for HA communication
- `MediaPlayer` entity: play/pause/stop/volume/mute (no next/prev — protocol gap)
- `template text_sensor` "Now Playing": publishes song title
- `template select` "Station": publishes and accepts station selection
- `button` entities for Next/Previous: workaround for missing `NEXT_TRACK`/`PREVIOUS_TRACK` in ESPHome's MediaPlayerCommand protobuf (aioesphomeapi stops at `TURN_OFF=13`)

## ESPHome Build System

### Build & Deploy Commands

```bash
cd esphome/
esphome compile esp32radio.yaml    # Build
esphome upload esp32radio.yaml --device /dev/ttyACM0  # Flash via USB
esphome upload esp32radio.yaml     # Flash via OTA (after first USB flash)
esphome logs esp32radio.yaml --device /dev/ttyACM0    # Serial logs
```

### PlatformIO (legacy + tests)

```bash
pio run -e esp32-s3-box             # Build legacy firmware
pio test -e native                  # Run native unit tests (always run before commit)
cd bt-bridge && pio run -e bt-bridge  # Build BT bridge firmware
```

### Critical Build Workarounds

These workarounds are in `__init__.py` files and MUST be preserved:

1. **`esp_driver_i2s` excluded by ESPHome** → Un-exclude via `ARDUINO_LIBRARY_IDF_COMPONENTS` AND `include_builtin_idf_component()`. Both mechanisms must be addressed.
2. **`esp_lcd` excluded by ESPHome** → `include_builtin_idf_component("esp_lcd")` in winamp_display's `__init__.py`.
3. **Arduino library headers not auto-discovered** → ESPHome sets `lib_ldf_mode=off`. Add `-I` flags for FFat, FS, SD, WiFi, Network, SPI, etc.
4. **`esp_dsp.h` missing** → Audio.h includes it for EQ/FFT we don't use. Stub header in `media_player/stubs/`.
5. **`-DCORE_DEBUG_LEVEL=2`** → Arduino macro not set by ESPHome framework. Required by some libraries.
6. **LovyanGFX defines** → `-DLGFX_USE_V1` and `-DLGFX_AUTODETECT` MUST be set via `cg.add_build_flag()`, NOT in headers (causes redefinition warnings).
7. **Platform component schemas** → `media_player`, `switch`, `i2c` CANNOT be listed in `DEPENDENCIES = [...]` — causes import errors. Use `DEPENDENCIES = ["network"]` or `["i2c"]` only for non-platform deps.

### Framework Versions

- ESPHome 2026.3.2, pioarduino stable
- ESP-IDF 5.5.3, Arduino 3.3.7
- LovyanGFX 1.2.19 (pinned via `cg.add_library`)

## ESPHome Pitfalls (Hard-Won Lessons)

### I2C Bus Conflict (CRITICAL)

ESPHome's `i2c:` component owns I2C bus 0 (GPIO 8/18). LovyanGFX `tft_.init()` tries to create its own I2C master → fails with "bus acquire failed" errors. This means:

- **`tft_.getTouch()` DOES NOT WORK** — LovyanGFX can't read GT911 via its internal I2C
- **`lgfx::i2c::transactionWriteRead()` fails during loop()** — works in setup() but ESPHome's bus recovery between setup and loop invalidates lgfx internal handles
- **Solution**: Use ESPHome's native I2C. WinampDisplay inherits `i2c::I2CDevice` and uses `read_register16()` for GT911's 16-bit register addresses.
- Display SPI init works fine — only I2C (touch, codec) is affected.

### GT911 Touch Coordinates

- **Raw GT911 register reads return screen-space coordinates** — no Y offset needed. The `+12` offset in the original firmware was for LovyanGFX `getTouch()` output, which applies additional transforms.
- Status register `0x814E`: bit 7 = data ready, bit 4 = home key, bits 3:0 = num touches
- Coordinate data at `0x8150`: `[x_low, x_high, y_low, y_high]`
- Must write `0x00` to `0x814E` after reading (3-byte write: `0x81, 0x4E, 0x00`)
- GT911 address varies (0x14 or 0x5D) depending on INT pin state at reset — probe at startup

### PSRAM Sprite Allocation

- LovyanGFX `createSprite()` defaults to **internal RAM** — not PSRAM
- MUST call `sprite.setPsram(true)` before `createSprite()` for sprites >~30KB
- Canvas 320×218×2 = 140KB needs PSRAM. Ticker (11KB) and viz (9KB) fit in internal RAM.

### Audio Sample Rate

- Different streams use different sample rates (44100, 48000, etc.)
- `I2SBridge::bridge_sample_rate` must be updated when sample rate changes
- Parse `"SampleRate (Hz):"` from `Audio::evt_info` callback and update the variable
- If not updated, the I2S bridge resampler won't engage → audio plays at wrong speed ("funny voice")

### Non-Blocking PA Enable

- The PA pin (GPIO 46) needs a ~200ms delay after codec init before enabling, to prevent speaker pop
- NEVER use `delay(200)` — it blocks the entire ESPHome main loop (WiFi, API, sensors, UI)
- Use a deferred state machine: set `pa_pending_ = true` + `pa_pending_ms_ = millis()`, check in `loop()`

### NVS Flash Wear

- Never save to NVS on every button press or slider drag — flash is rated ~100K erases per sector
- Use dirty-flag + debounced save pattern: `mark_vol_dirty_()` sets flag + timestamp, `flush_vol_if_dirty_()` writes after 3s of inactivity
- All volume paths (HA set, HA up/down, touch slider, touch buttons) must use this pattern

### ESPHome API Protocol Gaps

- `MediaPlayerCommand` protobuf stops at `TURN_OFF=13` — no `NEXT(14)`/`PREVIOUS(15)`
- `aioesphomeapi` PR #911 to add them was **Closed** (not merged)
- Use `button` entities as the workaround
- Do NOT add `NEXT_TRACK`/`PREVIOUS_TRACK` feature flags — they're misleading since commands can't be sent

### WiFi and Network APIs

- `WiFi.localIP()` returns `0.0.0.0` in ESPHome — use `network::get_ip_addresses()[0].str_to()` instead
- `WiFi.status()` doesn't work — use `network::is_connected()` (which is also inlined in ESPHome 2026.3+)
- ES8311 sample rate must be set to 44100Hz (not the default 16000Hz)

### Display Loop Timing

- Full-frame SPI push of 140KB at 40MHz takes 55-78ms per frame — exceeds ESPHome's 30ms warning threshold
- This is expected and does NOT affect audio (Core 0) or WiFi stability — ignore the "component took too long" warnings

## UI Styling (Winamp 2 Theme)

The UI follows a Winamp 2 classic skin aesthetic with pixel-precise beveled elements:

- **Title bar** (10px): Accent-colored "R" logo, grooved accent lines flanking faux-bold "ESP32 RADIO" title, three decorative window buttons (○ − ×). Content offset by `tm=4` (top) and `lm=6` (sides) for screen viewing angle.
- **Buttons** (`draw_wa_btn_()` method): Silver face (`WA_BTN_FACE` 0xC618), 1px dark border (`WA_BORDER`), top-left white highlight (`WA_HIGHLIGHT`), bottom-right gray shadow (`WA_SHADOW`). Pressed state replaces bevels with inner shadow rect and shifts text +1,+1.
- **State indicators**: 5×5 square in button top-left corner — green (`TFT_GREEN`) when active, gray (`WA_SHADOW`) when inactive. Used on MUTE and BT SPEAKER buttons.
- **Volume knob**: Winamp beveled rectangle matching button palette (not rounded).
- **WiFi signal**: 4 ascending bars (2/4/6/8px tall) next to WIFI label. RSSI thresholds: −50/−60/−70 dBm for 4/3/2/1 bars.
- **Touch highlight**: `touch_highlight_` index 0–4 (−, MUTE, +, slider, BT) with 200ms flash.
- **WA color constants** are `constexpr` (no `color565()` in draw loops): `WA_BTN_FACE`, `WA_HIGHLIGHT`, `WA_SHADOW`, `WA_BORDER`.

## Bluetooth Bridge (`bt-bridge/`)

Separate PlatformIO project for an ESP32-WROOM-32D that receives I2S audio from the main ESP32-S3 and streams it to a Bluetooth speaker via A2DP.

- **Architecture**: I2S0 slave RX → lock-free `PcmRingBuffer` → A2DP source callback
- **I2S wiring**: S3 GPIO 10→25 (BCLK), GPIO 14→26 (LRCK), GPIO 11→27 (DOUT→DIN), plus common GND
- **Config**: `bt-bridge/include/config.h` — `BT_SINK_NAME` (default `"JBL Flip 4"`), ring buffer size 2048 frames
- **DMA tuning**: I2S1 on the S3 side needs 16×480 DMA frames with 10ms write timeout to avoid drops
- ESP32-S3 does NOT support Bluetooth Classic (BR/EDR) — only BLE 5.0. A2DP requires the original ESP32.
- WiFi and BT Classic cannot coexist on a single ESP32 — hence the two-board wired I2S approach.
- `audio_process_i2s` override MUST be in a separate `.cpp` that doesn't include `Audio.h` — weak attribute taints any definition in the same translation unit.
- Never set `*continueI2S=false` in `audio_process_i2s` — it removes I2S0 blocking write pacing, causing the decoder to run at CPU speed and flood downstream buffers.
- Bridge includes linear interpolation resampler for non-44100Hz sources (e.g., BBC at 48kHz).
- Write 960 bytes of silence after `i2s_channel_enable()` to prevent BT speaker crack on reconnect.

## Coding Guidelines

- Use `char[]` with `strlcpy`/`snprintf` instead of Arduino `String` — avoids heap fragmentation on ESP32.
- Pre-compute `color565()` values in `setup()` — never call in draw loops.
- `audio.loop()` must run frequently on its dedicated core. Never put blocking code in the audio task.
- NVS persistence uses `markDirty()`/`flushIfDirty()` pattern — never save on every event.
- All audio callback events use `Audio::msg_t` with `msg.e` (event type) and `msg.msg` (string payload).
- Mark component classes `final` — enables ESPHome 2026.3+ devirtualization of `loop()`, `setup()`, etc.
- Use `network::is_connected()` and `network::get_ip_addresses()` — never raw `WiFi.*` APIs.

## Code Review Checklist

When reviewing changes, always verify:

- **Memory leaks**: No Arduino `String` in render loops, no `new`/`malloc` without matching `delete`/`free`, sprites not re-created without `deleteSprite()`.
- **Thread safety**: Never call `audio.*` methods (except `setVolume()`) from Core 1. Use `pending_connect_`/`pending_pause_`/`pending_stop_` flag pattern. Copy data (URL, title) into buffers BEFORE setting flags. Use double-buffer for shared strings. All cross-core shared state must be `volatile`.
- **NVS flash wear**: Never save to NVS in rapid-fire handlers (touch, buttons, slider). Use `mark_vol_dirty_()`/`flush_vol_if_dirty_()` for deferred writes with 3s debounce.
- **Blocking calls**: No `delay()` in ESPHome `loop()`. Use deferred state machines with `millis()` timestamps.
- **Draw loop cost**: No `color565()` calls, `WiFi.localIP()`, `network::get_ip_addresses()`, or heap-allocating functions inside render loops. Pre-compute in `setup()`.
- **Overflow**: `millis()` arithmetic must use unsigned subtraction. Intermediate touch/volume math must stay within `int` range.
- **I2S bridge**: `audio_process_i2s` must stay in its own `.cpp` without `Audio.h`. Never disable `continueI2S`. Update `bridge_sample_rate` when stream format changes.
- **I2C bus**: All GT911 reads must go through ESPHome's I2C bus (`read_register16`), never `lgfx::i2c` or `Wire`.
- **Touch zones**: Must match drawn button coordinates. GT911 raw coords = screen coords (no offset). Expand zones for buttons near screen edges.
- **UI consistency**: New buttons must use `draw_wa_btn_()` with the Winamp 2 palette. Update `touch_highlight_` comment when adding new indices.
- **Build workarounds**: Preserve all `__init__.py` IDF component un-exclusions and Arduino library includes. Test build after any `__init__.py` changes.
- **Documentation**: Update this file if changes affect features, architecture, pitfalls, or the review checklist.

## Development Workflow

Before completing any task, follow this checklist:

1. **Re-read**: Review the Code Review Checklist and ESPHome Pitfalls sections above
2. **Build**: `cd esphome && esphome compile esp32radio.yaml` — compiles without errors
3. **Test**: `pio test -e native` — all unit tests pass (run from repo root)
4. **Flash & verify**: `esphome upload esp32radio.yaml --device /dev/ttyACM0` — test on hardware
5. **Code review**: Review the diff (`git diff`) against the checklist above
6. **Update docs**: Update this file if the change affects features, architecture, pitfalls, or the checklist
7. **Commit**: Write a descriptive commit message summarizing all changes
