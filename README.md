# ESP32-S3-BOX-3 Internet Radio

Internet radio for the [ESP32-S3-BOX-3](https://github.com/espressif/esp-box) built on ESPHome with custom external components. Streams internet radio stations with a Winamp 2 themed touch UI, real-time FFT spectrum visualizer, optional Bluetooth speaker output, and full Home Assistant integration.

## Features

- 10 pre-configured internet radio stations (electronic, trance, rock, metal, pop, news)
- Winamp 2 inspired UI: beveled buttons, grooved title bar, signal bars, state indicators
- Real-time 16-band FFT spectrum visualizer (segmented gradient bars)
- Touch screen station selection & volume control
- Hardware buttons: Boot (cycle brightness), Mute switch (GPIO 1), Home (stop/play)
- Auto-dim screen when stopped, restore on play
- Scrolling song title ticker with ID3/ICY metadata (Artist - Title)
- WiFi signal strength & bitrate display
- Bluetooth speaker output via I2S bridge to ESP32-WROOM-32D
- Home Assistant integration via ESPHome native API (media player, controls, sensors)
- Station, volume, brightness & output mode persisted across reboots
- OTA firmware updates

## Quick Start

### 1. Install ESPHome

```bash
pip install esphome
```

### 2. Configure Secrets

```bash
cd esphome
cp secrets.yaml.sample secrets.yaml  # edit with your WiFi/API credentials
```

`secrets.yaml` should contain:
```yaml
wifi_ssid: "YourSSID"
wifi_password: "YourPassword"
api_key: "your-api-encryption-key"
ota_password: "your-ota-password"
```

### 3. Build & Flash

```bash
cd esphome
esphome compile esp32radio.yaml                          # build
esphome upload esp32radio.yaml --device /dev/ttyACM0     # flash via USB
esphome upload esp32radio.yaml                           # flash via OTA (after first USB flash)
esphome logs esp32radio.yaml --device /dev/ttyACM0       # serial logs
```

## Testing with Copilot remote agent

This repository includes `.github/workflows/copilot-setup-steps.yml` to prepare Copilot cloud agent with build tools and run smoke builds:

- `esphome compile esp32radio.yaml`
- `pio run -e bt-bridge --project-dir bt-bridge`

To avoid TLS download failures in corporate/proxied networks, set these optional values as repository- or organization-level Actions secrets/variables (not in an environment unless the workflow is updated to use one):

- `HTTPS_PROXY`, `HTTP_PROXY`, `NO_PROXY` (secret or variable)
- `COPILOT_CA_CERT_PEM` (secret or variable containing your PEM CA chain)

The setup workflow writes `COPILOT_CA_CERT_PEM` to a temp file and exports:
`SSL_CERT_FILE`, `REQUESTS_CA_BUNDLE`, `CURL_CA_BUNDLE`, `NODE_EXTRA_CA_CERTS`, and `ssl_cert_file`.

After merging this workflow to the default branch, you can validate it from **Actions** by running **Copilot Setup Steps** manually.

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
| Touch BT SPEAKER   | Toggle Bluetooth/local speaker (green indicator = active) |
| Home button (red ●)| Stop/play toggle               |
| Boot button (GPIO0)| Cycle screen brightness        |
| Mute switch (GPIO1)| Toggle mute                    |

## Hardware — ESP32-S3-BOX-3

| Peripheral         | Pin(s)                             |
|--------------------|------------------------------------|
| Display (ILI9342C) | SCK=7, MOSI=6, CS=5, DC=4, BL=47  |
| I2S0 Audio (ES8311)| MCLK=2, BCLK=17, LRCK=45, DOUT=15 |
| I2S1 Bridge Output | BCLK=10, LRCK=14, DOUT=11         |
| I2C bus            | SDA=8, SCL=18                      |
| Touch (GT911)      | INT=3 (shared I2C bus)             |
| Power amplifier    | GPIO 46                            |
| Home button (red ●)| GT911 soft key (I2C reg 0x814E)    |
| Boot button        | GPIO 0                             |
| Mute switch        | GPIO 1                             |

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

The radio appears as a native ESPHome media player in Home Assistant — no MQTT required.

### Entities

| Entity | Type | Description |
|--------|------|-------------|
| ESP32 Radio | Media Player | Play/pause/stop/volume/mute + play_media for arbitrary URLs |
| Station | Select | Station picker with all configured stations |
| Now Playing | Text Sensor | Current song (Artist - Title) |
| Screen Brightness | Number | Display brightness (1-255) |
| BT Speaker | Switch | Toggle Bluetooth/local speaker output |
| Next/Previous Station | Buttons | Station navigation |

### Setup

1. ESPHome device auto-discovers in HA (Settings → Devices → ESP32 Radio)
2. All entities appear automatically via the native API
3. No MQTT broker required

## Architecture

- **Core 0**: Dedicated FreeRTOS audio task — `audio.loop()` runs continuously for uninterrupted playback
- **Core 1**: ESPHome main loop — UI rendering at ~15fps, touch/button input, FFT computation, WiFi, HA API
- **Display**: PSRAM-backed LovyanGFX sprite (320×218), composed and pushed once per frame
- **Audio**: ES8311 I2C codec (via ESPHome `audio_dac`), ESP32-audioI2S library for HTTP streaming
- **Platform APIs**: Custom components prefer ESP-IDF/FreeRTOS APIs (GPIO, reboot delay/restart) to minimize direct Arduino API usage outside the streaming library
- **FFT**: 128-point radix-2 FFT — Core 0 captures samples (double-buffered), Core 1 computes spectrum
- **I2S Bridge**: `audio_process_i2s()` hook sends decoded PCM to I2S1 TX when BT mode is active
- **HA Integration**: ESPHome native API with publish-on-change (zero polling)

## Project Structure

```
├── esp32radio.yaml                  # Main ESPHome config
├── secrets.yaml                     # WiFi/API credentials (gitignored)
├── components/
│   ├── internet_radio/              # Media player component (audio streaming)
│   ├── i2s_bridge/                  # I2S bridge switch (BT speaker output)
│   └── winamp_display/              # Winamp 2 display, touch, FFT visualizer
├── bt-bridge/                       # ESP32-WROOM-32D BT A2DP bridge firmware
└── archive/
    └── platformio/                  # Legacy PlatformIO firmware (reference only)
```

## License

ES8311 driver: Apache-2.0 (Espressif Systems). Application code: MIT.
