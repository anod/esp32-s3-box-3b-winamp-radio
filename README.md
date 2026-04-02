# ESP32-S3-BOX-3 Internet Radio

Internet radio firmware for the [ESP32-S3-BOX-3](https://github.com/espressif/esp-box) (ESP32-S3, ILI9342C 320×240 display, ES8311 audio codec, GT911 touch). Streams internet radio stations with a retro-styled touch UI, optional Bluetooth speaker output, and Home Assistant integration.

Based on [VolosR/WaveshareRadioStream](https://github.com/VolosR/WaveshareRadioStream), adapted for ESP32-S3-BOX-3 hardware.

## Features

- 10 pre-configured internet radio stations (electronic, trance, rock, metal, pop, news)
- Touch screen station selection & volume control
- Hardware buttons: Boot (next station), Mute (GPIO 1)
- Scrolling song title ticker with ID3/ICY metadata
- Real FFT spectrum visualizer (Winamp 2 style segmented bars)
- WiFi signal strength & bitrate display
- Bluetooth speaker output via I2S bridge to ESP32-WROOM-32D
- Home Assistant integration via MQTT (auto-discovery, media player entity)
- Station, volume & output mode persisted across reboots

## Quick Start

### 1. Install PlatformIO

```bash
pip install platformio
```

Ensure `~/.local/bin` is on your PATH. The first build auto-downloads the toolchain (~1 GB).

> **Important:** This project requires the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform (Arduino core 3.x / ESP-IDF 5.x), already configured in `platformio.ini`.

### 2. Configure WiFi & MQTT

```bash
cp .env.sample .env   # then edit with your SSID/password and optional MQTT settings
```

### 3. Build & Flash

```bash
pio run                # build only
pio run -t upload      # build + flash
pio device monitor     # serial monitor (115200 baud)
```

### 4. Run Tests

```bash
pio test -e native     # host-side unit tests (no hardware required)
```

### Arduino IDE (alternative)

Install **esp32 by Espressif Systems** board package **v3.x** via Board Manager. Select board **ESP32-S3-BOX-3**, enable **USB CDC On Boot** and **OPI PSRAM**. Copy `.env` values into `include/config.h` manually (the `load_env.py` script is PlatformIO-only).

### WSL USB passthrough

WSL can't see USB devices natively. Install [usbipd-win](https://github.com/dorssel/usbipd-win) on **Windows** (`winget install usbipd`), then:

```powershell
usbipd list                          # find the ESP32-S3 bus ID
usbipd bind --busid <BUSID>          # first time only
usbipd attach --wsl --busid <BUSID>  # re-run after each device reset
```

In WSL, grant serial access once: `sudo usermod -aG dialout $USER` (re-login required).

## Controls

| Input              | Action                         |
|--------------------|--------------------------------|
| Touch station name | Switch to that station         |
| Touch VOL −/+      | Decrease/increase volume       |
| Touch MUTE         | Toggle mute                    |
| Touch BT/SPK       | Toggle Bluetooth/local speaker |
| Boot button (GPIO0)| Next station                   |
| Mute button (GPIO1)| Toggle mute                    |

## Configuration

Settings can be overridden via `.env` (see `.env.sample`) or edited directly in `include/config.h`.

| Option           | Default                | Description                    |
|------------------|------------------------|--------------------------------|
| `WIFI_SSID`      | —                      | WiFi network name              |
| `WIFI_PASSWORD`   | —                      | WiFi password                  |
| `DEFAULT_VOLUME`  | `15`                   | Initial volume (0–21)          |
| `MAX_VOLUME`      | `21`                   | Maximum volume level           |
| `MQTT_BROKER`     | `homeassistant.local`  | MQTT broker hostname           |
| `MQTT_PORT`       | `1883`                 | MQTT broker port               |
| `MQTT_USER`       | (empty)                | MQTT username (optional)       |
| `MQTT_PASSWORD`   | (empty)                | MQTT password (optional)       |

## Hardware — ESP32-S3-BOX-3

| Peripheral         | Pin(s)                             |
|--------------------|------------------------------------|
| Display (ILI9342C) | SCK=7, MOSI=6, CS=5, DC=4, BL=47  |
| I2S0 Audio (ES8311)| MCLK=2, BCLK=17, LRCK=45, DOUT=15 |
| I2S1 Bridge Output | BCLK=10, LRCK=14, DOUT=11         |
| I2C bus            | SDA=8, SCL=18                      |
| Touch (GT911)      | INT=3 (shared I2C bus)             |
| Power amplifier    | GPIO 46                            |
| Boot button        | GPIO 0                             |
| Mute button        | GPIO 1                             |

## Bluetooth Speaker Output

The ESP32-S3 does not support Bluetooth Classic (A2DP) — only BLE 5.0. To stream audio to a Bluetooth speaker, a second ESP32-WROOM-32D board acts as an I2S-to-A2DP bridge.

### How It Works

1. The main ESP32-S3 decodes the MP3 stream as normal
2. The `audio_process_i2s()` hook siphons decoded PCM to I2S1 TX (when BT mode is active)
3. The WROOM-32D receives audio on I2S0 as a slave, buffers it in a lock-free ring buffer, and streams to a Bluetooth speaker via A2DP

### Wiring (3 signal wires + GND)

| ESP32-S3 (I2S1 TX) | ESP32-WROOM-32D (I2S0 RX) |
|---------------------|----------------------------|
| GPIO 10 (BCLK)     | GPIO 25 (BCLK)            |
| GPIO 14 (LRCK)     | GPIO 26 (LRCK)            |
| GPIO 11 (DOUT)     | GPIO 27 (DIN)             |
| GND                 | GND                        |

### BT Bridge Firmware

The bridge firmware lives in `bt-bridge/`. Configure the target Bluetooth speaker name in `bt-bridge/.env` (or `bt-bridge/include/config.h`, default: `JBL Flip 4`).

```bash
cd bt-bridge
cp .env.sample .env       # set BT_SINK_NAME
pio run -e bt-bridge      # build
pio run -e bt-bridge -t upload --upload-port /dev/ttyUSB0  # flash
```

## Home Assistant Integration

The radio appears as a full media player in Home Assistant via MQTT auto-discovery.

### Setup

1. Configure MQTT broker details in `.env` (see Configuration above)
2. The radio publishes HA discovery on connect — entities appear automatically
3. Optionally add `ha/media_player.yaml` to your HA config for a unified media player entity

### MQTT Topics

| Topic                                      | Direction | Description                    |
|--------------------------------------------|-----------|--------------------------------|
| `esp32radio/state`                         | Publish   | JSON state (retained)          |
| `esp32radio/availability`                  | Publish   | `online`/`offline` (LWT)       |
| `esp32radio/cmd/{volume,mute,source,...}`  | Subscribe | Commands from HA               |
| `homeassistant/device/esp32radio/config`   | Publish   | HA device discovery (retained) |

### Exposed Entities

- **Select**: Station picker (all configured stations)
- **Number**: Volume (0–100%)
- **Switch**: Mute toggle
- **Sensors**: Now Playing, State, Bitrate, WiFi Signal (dBm)
- **Buttons**: Play, Pause, Stop, Next, Previous

### Universal Media Player

Copy `ha/media_player.yaml` into your HA configuration to combine the individual MQTT entities into a single `media_player` entity with full transport controls, source selection, and volume management.

## Adding Stations

Edit the `stationUrls[]` and `stationNames[]` arrays in `src/main.cpp`. Update `NUM_STATIONS` to match. The UI supports up to 10 stations in the visible list. Station names also appear in the MQTT select entity — restart the device to update HA discovery.

## Architecture

- **Core 0**: Audio streaming task — `audio.loop()` runs continuously for uninterrupted playback
- **Core 1**: Main loop — UI rendering at ~15fps, touch/button input, MQTT polling
- **Display**: Single PSRAM-backed sprite (320×240), composed and pushed once per frame
- **Audio**: ES8311 I2C codec driver, ESP32-audioI2S library for HTTP MP3 streaming
- **I2S Bridge**: `audio_process_i2s()` hook sends decoded PCM to I2S1 TX when BT mode is active
- **MQTT**: PubSubClient with HA device-based auto-discovery and JSON state publishing

## Project Structure

```
├── src/
│   ├── main.cpp          # Application entry point, UI, audio callbacks
│   ├── mqtt.cpp          # MQTT broker connection, HA discovery, commands
│   ├── i2s_bridge.cpp    # I2S1 TX bridge for BT output
│   └── es8311.cpp        # ES8311 codec I2C driver
├── include/
│   ├── config.h          # WiFi, MQTT, audio defaults
│   ├── mqtt.h            # MQTT public API and constants
│   └── pins.h            # Pin definitions for all peripherals
├── bt-bridge/            # ESP32-WROOM-32D BT A2DP bridge firmware
├── ha/
│   └── media_player.yaml # HA Universal Media Player config
├── test/
│   └── test_native/      # Host-side unit tests (Unity)
└── boards/
    └── esp32-s3-box.json # Custom board definition
```

## License

ES8311 driver: Apache-2.0 (Espressif Systems). Application code: MIT.
