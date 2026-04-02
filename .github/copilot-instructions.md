# Copilot Instructions — ESP32-S3-BOX-3 Internet Radio

## Project Overview

PlatformIO Arduino firmware for the ESP32-S3-BOX-3 development board. Streams internet radio via HTTP MP3 with a touch-screen UI. Supports Bluetooth speaker output via an I2S bridge to an ESP32-WROOM-32D, and Home Assistant integration via MQTT.

## Hardware

- **Board**: ESP32-S3-BOX-3 (ESP32-S3, 16MB flash, 8MB OPI PSRAM)
- **Display**: ILI9342C 320×240 SPI (managed by LovyanGFX autodetect)
- **Audio**: ES8311 I2C codec → I2S0 → built-in speaker (PA on GPIO 46)
- **Touch**: GT911 capacitive controller (shared I2C bus with codec)
- **I2S bridge output**: I2S1 TX on GPIO 10/14/11 → wired to ESP32-WROOM-32D for Bluetooth A2DP
- **Critical pin note**: BOX-3 uses I2S_LRCK=GPIO45, LCD_BL=GPIO47. BOX v1 has these swapped — do NOT mix them.

## Architecture

- **Core 0**: Dedicated FreeRTOS audio task (`audioTask`) — calls `audio.loop()` continuously, consumes `pendingConnect`/`pendingPause`/`pendingStop` flags
- **Core 1**: Main Arduino `loop()` — UI rendering (~15fps), touch/button input, WiFi status, MQTT polling
- **Display**: Single 320×240 PSRAM-backed sprite, fully composed and pushed once per frame. No partial redraws or multiple sprites — this prevents flicker.
- **Audio callbacks** run on Core 0 and write to shared state (`songTitle`, `bitrate`, `playState`). The render loop on Core 1 reads these. Use `volatile` for simple types; char arrays are tolerated without mutex since brief inconsistency is acceptable for display.
- **I2S bridge** (`src/i2s_bridge.cpp`): Overrides the `audio_process_i2s()` weak hook to siphon decoded PCM to I2S1 TX when `btMode` is enabled. Must be in a separate `.cpp` that does NOT include `Audio.h` — weak attribute taints any definition in the same translation unit.
- **MQTT module** (`src/mqtt.cpp`): Owns broker connection, HA discovery, state publishing, and command parsing. Commands are queued via `mqttCmdPending` and consumed by `loop()` on Core 1, which sets pending flags for Core 0 audio actions.

### Cross-Core Communication

Audio-library methods (`connecttohost`, `pauseResume`, `stopSong`) must only be called from Core 0. Core 1 (UI/MQTT) sets volatile flags that `audioTask` consumes:

- `pendingConnect` — triggers `audio.connecttohost()` for station changes
- `pendingPause` — triggers `audio.pauseResume()` for play/pause toggle
- `pendingStop` — triggers `audio.stopSong()` for stop commands
- Only `audio.setVolume()` is safe to call cross-core.

## Build System

- **Platform**: pioarduino (Arduino core 3.x / ESP-IDF 5.x) — NOT the official espressif32 platform
- **WiFi/MQTT credentials**: injected via `.env` file → `load_env.py` pre-script → `-D` build flags. Never hardcode credentials.
- **Board JSON**: `boards/esp32-s3-box.json` defines the `ARDUINO_ESP32_S3_BOX` flag which triggers LovyanGFX autodetect for BOX-3.
- **Main firmware**: `pio run -e esp32-s3-box` (build), `pio run -t upload --upload-port /dev/ttyACM0` (flash)
- **BT bridge firmware**: `cd bt-bridge && pio run -e bt-bridge` (build), `pio run -e bt-bridge -t upload --upload-port /dev/ttyUSB0` (flash)
- **Native unit tests**: `pio test -e native` — runs host-side tests (Unity framework) without hardware

## Key Libraries

- **LovyanGFX** (`LGFX_AUTODETECT`): Display and touch. Autodetect selects `ESP32_S3_BOX_V3` profile. Do NOT use manual LGFX configuration.
- **ESP32-audioI2S**: HTTP streaming + MP3 decode + I2S output. Uses `std::function` callback API (`Audio::audio_info_callback`), NOT the old weak-linked `audio_info()` free functions.
- **PubSubClient**: MQTT client for Home Assistant integration. Broker config via `.env`.
- **ESP32-A2DP** (bt-bridge only): Bluetooth A2DP source on ESP32-WROOM-32D. ESP32-S3 does NOT support Bluetooth Classic — a second ESP32 board is required.
- **ES8311 driver** (`src/es8311.cpp`): Custom I2C driver using Arduino Wire. Apache-2.0 licensed from Espressif.

## Bluetooth Bridge (`bt-bridge/`)

Separate PlatformIO project for an ESP32-WROOM-32D that receives I2S audio from the main ESP32-S3 and streams it to a Bluetooth speaker via A2DP.

- **Architecture**: I2S0 slave RX → lock-free `PcmRingBuffer` → A2DP source callback
- **I2S wiring**: S3 GPIO 10→25 (BCLK), GPIO 14→26 (LRCK), GPIO 11→27 (DOUT→DIN), plus common GND
- **Config**: `bt-bridge/include/config.h` — `BT_SINK_NAME` (default `"JBL Flip 4"`), ring buffer size 2048 frames
- **DMA tuning**: I2S1 on the S3 side needs 6×480=2880 DMA frames with 10ms write timeout to avoid drops
- ESP32-S3 does NOT support Bluetooth Classic (BR/EDR) — only BLE 5.0. A2DP requires the original ESP32.
- WiFi and BT Classic cannot coexist on a single ESP32 — hence the two-board wired I2S approach.

## MQTT / Home Assistant Integration

### Module Structure

- `include/mqtt.h` — public API, topic/entity constants
- `src/mqtt.cpp` — broker connection, HA discovery, state publishing, command handling
- `ha/media_player.yaml` — Universal Media Player wrapper config for HA
- `test/test_native/test_mqtt.cpp` — native unit tests (volume conversion, station lookup, JSON serialization)

### Topics

All topics prefixed with `esp32radio/`:

- `esp32radio/state` — retained JSON state (volume, mute, source, media_title, bitrate, output, rssi, ip)
- `esp32radio/availability` — `online`/`offline` (LWT)
- `esp32radio/cmd/{volume,mute,source,play,pause,stop,next,prev}` — command subscriptions
- `homeassistant/device/esp32radio/config` — HA device-based discovery (11 entities: select, number, switch, sensors, buttons)

### State JSON Fields

`state`, `volume` (0.0–1.0), `is_volume_muted`, `media_title`, `source`, `bitrate`, `output` (`local`|`bt`), `rssi`, `ip`

### Config

Set via `.env` → build flags: `MQTT_BROKER`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASSWORD`. Defaults: `homeassistant.local:1883`, no auth.

## Coding Guidelines

- Use `char[]` with `strlcpy`/`snprintf` instead of Arduino `String` — avoids heap fragmentation on ESP32.
- Pre-compute `color565()` values in `setup()` — never call in draw loops.
- Cache `WiFi.localIP().toString()` — don't call it every frame.
- `audio.loop()` must run frequently on its dedicated core. Never put blocking code in the audio task.
- Station, volume, and `btMode` persist in NVS via `Preferences`. Use `markDirty()`/`flushIfDirty()` for deferred writes.
- All audio callback events use `Audio::msg_t` with `msg.e` (event type) and `msg.msg` (string payload).
- Call `mqttNotifyStateChange()` whenever player state changes (station, volume, playback, metadata) — this sets the dirty flag for MQTT publishing.

## Common Pitfalls

- HTTPS streams fail with SSL memory allocation errors on ESP32 — use HTTP only.
- `Wire.begin()` for ES8311 conflicts with LovyanGFX autodetect I2C init (harmless warning: "bus already initialized").
- The PA pin (GPIO 46) must stay LOW during boot/codec init to prevent speaker noise. Enable after stream connects. Driven LOW when `btMode` is active.
- Touch coordinates from GT911 autodetect have a Y offset (~+12px) — applied in `handleTouch()`.
- `audio_process_i2s` override MUST be in a separate `.cpp` that doesn't include `Audio.h` — weak attribute taints any definition in the same translation unit.
- Never set `*continueI2S=false` in `audio_process_i2s` — it removes I2S0 blocking write pacing, causing the decoder to run at CPU speed and flood downstream buffers.
- ESP32 WiFi and BT Classic (A2DP) cannot coexist — single shared 2.4 GHz radio. Use wired I2S between ESP32s.

## Code Review Checklist

When reviewing changes, always verify:

- **Memory leaks**: No Arduino `String` in render loops, no `new`/`malloc` without matching `delete`/`free`, NVS handles closed with `prefs.end()`, sprites not re-created without `deleteSprite()`.
- **Thread safety**: Never call `audio.*` methods (except `setVolume()`) from Core 1. Use `pendingConnect`/`pendingPause`/`pendingStop` flag pattern for cross-core audio commands. All cross-core shared state must be `volatile`.
- **MQTT integration**: State changes must call `mqttNotifyStateChange()`. Command handlers must validate inputs (clamp volume, check station names). Discovery JSON must fit the 1024-byte PubSubClient buffer.
- **NVS flash wear**: Never call `saveStation()` in rapid-fire handlers (touch, buttons). Use `markDirty()`/`flushIfDirty()` for deferred writes.
- **Draw loop cost**: No `color565()` calls, `WiFi.localIP().toString()`, or heap-allocating functions inside render loops. Pre-compute in `setup()`.
- **Overflow**: `millis()` arithmetic must use unsigned subtraction. Intermediate touch/volume math must stay within `int` range.
- **I2S bridge**: `audio_process_i2s` must stay in its own `.cpp` without `Audio.h`. Never disable `continueI2S`.
