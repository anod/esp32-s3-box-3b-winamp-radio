# ESP32-S3-BOX Internet Radio

Internet radio firmware for the [ESP32-S3-BOX (v1)](https://github.com/espressif/esp-box) (ESP32-S3, ILI9342C 320×240 display, ES8311 audio codec, TT21100 touch). Streams internet radio stations with a retro-styled touch UI.

Based on [VolosR/WaveshareRadioStream](https://github.com/VolosR/WaveshareRadioStream), adapted for ESP32-S3-BOX hardware.

## Features

- 8 pre-configured internet radio stations
- Touch screen station selection & volume control
- Hardware buttons: Boot (next station), Mute (toggle)
- Scrolling song title ticker
- WiFi signal strength & bitrate display
- Animated visualiser bars

## Quick Start

### 1. Install PlatformIO

```bash
pip install platformio
```

Ensure `~/.local/bin` is on your PATH. The first build auto-downloads the toolchain (~1 GB).

> **Important:** This project requires the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform (Arduino core 3.x / ESP-IDF 5.x), already configured in `platformio.ini`.

### 2. Configure WiFi

```bash
cp .env.sample .env   # then edit with your SSID/password
```

### 3. Build & Flash

```bash
pio run                # build only
pio run -t upload      # build + flash
pio device monitor     # serial monitor (115200 baud)
```

### Arduino IDE (alternative)

Install **esp32 by Espressif Systems** board package **v3.x** via Board Manager. Select board **ESP32-S3-BOX**, enable **USB CDC On Boot** and **OPI PSRAM**. Copy `.env` values into `include/config.h` manually (the `load_env.py` script is PlatformIO-only).

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
| Boot button (GPIO0)| Next station                   |
| Mute button (GPIO1)| Toggle mute                    |

## Configuration (`config.h`)

Settings can be overridden via `.env` (see `.env.sample`) or edited directly in `include/config.h`.

| Option          | Default | Description              |
|-----------------|---------|--------------------------|
| `WIFI_SSID`     | —       | WiFi network name        |
| `WIFI_PASSWORD`  | —       | WiFi password            |
| `DEFAULT_VOLUME` | `10`    | Initial volume (0–21)    |
| `MAX_VOLUME`     | `21`    | Maximum volume level     |

## Hardware — ESP32-S3-BOX (v1)

| Peripheral         | Pin(s)                           |
|--------------------|----------------------------------|
| Display (ILI9342C) | SCK=7, MOSI=6, CS=5, DC=4, RST=48, BL=45 |
| I2S Audio (ES8311) | MCLK=2, BCLK=17, LRCK=47, DOUT=15 |
| I2C bus            | SDA=8, SCL=18                    |
| Touch (TT21100)    | INT=3 (shared I2C bus)           |
| Power amplifier    | GPIO 46                          |
| Boot button        | GPIO 0                           |
| Mute button        | GPIO 1                           |

## Adding Stations

Edit the `stationUrls[]` and `stationNames[]` arrays in `src/main.cpp`. Update `NUM_STATIONS` to match. The UI supports up to 9 stations in the visible list.

## License

ES8311 driver: Apache-2.0 (Espressif Systems). Application code: MIT.
