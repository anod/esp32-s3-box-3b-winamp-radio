# Copilot Instructions — ESP32-S3-BOX-3 Internet Radio

## Project Overview

PlatformIO Arduino firmware for the ESP32-S3-BOX-3 development board. Streams internet radio via HTTP MP3 with a touch-screen UI.

## Hardware

- **Board**: ESP32-S3-BOX-3 (ESP32-S3, 16MB flash, 8MB OPI PSRAM)
- **Display**: ILI9342C 320×240 SPI (managed by LovyanGFX autodetect)
- **Audio**: ES8311 I2C codec → I2S → built-in speaker (PA on GPIO 46)
- **Touch**: GT911 capacitive controller (shared I2C bus with codec)
- **Critical pin note**: BOX-3 uses I2S_LRCK=GPIO45, LCD_BL=GPIO47. BOX v1 has these swapped — do NOT mix them.

## Architecture

- **Core 0**: Dedicated FreeRTOS audio task (`audioTask`) — calls `audio.loop()` continuously
- **Core 1**: Main Arduino `loop()` — UI rendering (~15fps), touch/button input, WiFi status
- **Display**: Single 320×240 PSRAM-backed sprite, fully composed and pushed once per frame. No partial redraws or multiple sprites — this prevents flicker.
- **Audio callbacks** run on Core 0 and write to shared state (`songTitle`, `bitrate`). The render loop on Core 1 reads these. Use `volatile` for simple types; char arrays are tolerated without mutex since brief inconsistency is acceptable for display.

## Build System

- **Platform**: pioarduino (Arduino core 3.x / ESP-IDF 5.x) — NOT the official espressif32 platform
- **WiFi credentials**: injected via `.env` file → `load_env.py` pre-script → `-D` build flags. Never hardcode credentials.
- **Board JSON**: `boards/esp32-s3-box.json` defines the `ARDUINO_ESP32_S3_BOX` flag which triggers LovyanGFX autodetect for BOX-3.

## Key Libraries

- **LovyanGFX** (`LGFX_AUTODETECT`): Display and touch. Autodetect selects `ESP32_S3_BOX_V3` profile. Do NOT use manual LGFX configuration.
- **ESP32-audioI2S**: HTTP streaming + MP3 decode + I2S output. Uses `std::function` callback API (`Audio::audio_info_callback`), NOT the old weak-linked `audio_info()` free functions.
- **ES8311 driver** (`src/es8311.cpp`): Custom I2C driver using Arduino Wire. Apache-2.0 licensed from Espressif.

## Coding Guidelines

- Use `char[]` with `strlcpy`/`snprintf` instead of Arduino `String` — avoids heap fragmentation on ESP32.
- Pre-compute `color565()` values in `setup()` — never call in draw loops.
- Cache `WiFi.localIP().toString()` — don't call it every frame.
- `audio.loop()` must run frequently on its dedicated core. Never put blocking code in the audio task.
- Station and volume persist in NVS via `Preferences`. Call `saveStation()` after changes.
- All audio callback events use `Audio::msg_t` with `msg.e` (event type) and `msg.msg` (string payload).

## Common Pitfalls

- HTTPS streams fail with SSL memory allocation errors on ESP32 — use HTTP only.
- `Wire.begin()` for ES8311 conflicts with LovyanGFX autodetect I2C init (harmless warning: "bus already initialized").
- The PA pin (GPIO 46) must stay LOW during boot/codec init to prevent speaker noise. Enable after stream connects.
- Touch coordinates from GT911 autodetect have a Y offset (~+12px) — applied in `handleTouch()`.

## Code Review Checklist

When reviewing changes, always verify:

- **Memory leaks**: No Arduino `String` in render loops, no `new`/`malloc` without matching `delete`/`free`, NVS handles closed with `prefs.end()`, sprites not re-created without `deleteSprite()`.
- **Thread safety**: Never call `audio.*` methods (except `setVolume()`) from Core 1. Use `pendingConnect` flag pattern for cross-core audio commands. All cross-core shared state must be `volatile`.
- **NVS flash wear**: Never call `saveStation()` in rapid-fire handlers (touch, buttons). Use `markDirty()`/`flushIfDirty()` for deferred writes.
- **Draw loop cost**: No `color565()` calls, `WiFi.localIP().toString()`, or heap-allocating functions inside render loops. Pre-compute in `setup()`.
- **Overflow**: `millis()` arithmetic must use unsigned subtraction. Intermediate touch/volume math must stay within `int` range.
