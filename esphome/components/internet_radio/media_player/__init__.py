"""ESPHome media_player platform: internet_radio."""
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import media_player, esp32, text_sensor, select
from esphome.components.esp32 import ARDUINO_LIBRARY_IDF_COMPONENTS
from esphome.components.esp32.const import KEY_ESP32, KEY_ARDUINO_LIBRARIES
from esphome.const import CONF_ID
from esphome.core import CORE
from esphome.components.internet_radio import internet_radio_ns

DEPENDENCIES = ["network"]

InternetRadio = internet_radio_ns.class_(
    "InternetRadio", media_player.MediaPlayer, cg.Component
)

CONF_I2S_BCLK_PIN = "i2s_bclk_pin"
CONF_I2S_LRCLK_PIN = "i2s_lrclk_pin"
CONF_I2S_DOUT_PIN = "i2s_dout_pin"
CONF_I2S_MCLK_PIN = "i2s_mclk_pin"
CONF_PA_PIN = "pa_pin"
CONF_DEFAULT_VOLUME = "default_volume"
CONF_NOW_PLAYING = "now_playing_id"
CONF_STATION_SELECT = "station_select_id"

CONFIG_SCHEMA = (
    media_player.media_player_schema(InternetRadio)
    .extend(
        {
            cv.Required(CONF_I2S_BCLK_PIN): cv.int_range(min=0, max=48),
            cv.Required(CONF_I2S_LRCLK_PIN): cv.int_range(min=0, max=48),
            cv.Required(CONF_I2S_DOUT_PIN): cv.int_range(min=0, max=48),
            cv.Optional(CONF_I2S_MCLK_PIN, default=2): cv.int_range(min=0, max=48),
            cv.Optional(CONF_PA_PIN, default=-1): cv.int_range(min=-1, max=48),
            cv.Optional(CONF_DEFAULT_VOLUME, default=15): cv.int_range(min=0, max=21),
            cv.Optional(CONF_NOW_PLAYING): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_STATION_SELECT): cv.use_id(select.Select),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_player.register_media_player(var, config)

    # ESP32-audioI2S needs esp_driver_i2s (excluded by default in ESPHome).
    # Un-exclude it from both cmake EXCLUDE_COMPONENTS and idf_component.yml stubs.
    idf_deps = ("esp_driver_i2s",)
    ARDUINO_LIBRARY_IDF_COMPONENTS["ESP32-audioI2S"] = idf_deps
    CORE.data[KEY_ESP32][KEY_ARDUINO_LIBRARIES].add("ESP32-audioI2S")
    for dep in idf_deps:
        esp32.include_builtin_idf_component(dep)

    # Enable Arduino libraries that Audio.h includes unconditionally.
    # ESPHome disables them by default (CONFIG_ARDUINO_SELECTIVE_*).
    for lib in ("FFat", "SD", "SD_MMC", "WiFi", "NetworkClientSecure", "Network", "FS", "SPI"):
        CORE.data[KEY_ESP32][KEY_ARDUINO_LIBRARIES].add(lib)

    # ESPHome sets lib_ldf_mode=off, so Arduino framework library headers
    # aren't auto-discovered. Add -I flags for libraries Audio.h needs.
    arduino_fw = Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32"
    for lib in ("FFat", "FS", "SD", "SD_MMC", "WiFi", "Network", "NetworkClientSecure", "SPI"):
        lib_src = arduino_fw / "libraries" / lib / "src"
        if lib_src.exists():
            cg.add_build_flag(f"-I{lib_src}")

    # esp_dsp stub: Audio.h includes esp_dsp.h but we don't use EQ/FFT.
    # Point -I at our stub directory BEFORE the real (non-existent) one.
    stubs_dir = Path(__file__).parent / "stubs"
    cg.add_build_flag(f"-I{stubs_dir}")

    cg.add(var.set_i2s_bclk_pin(config[CONF_I2S_BCLK_PIN]))
    cg.add(var.set_i2s_lrclk_pin(config[CONF_I2S_LRCLK_PIN]))
    cg.add(var.set_i2s_dout_pin(config[CONF_I2S_DOUT_PIN]))
    cg.add(var.set_i2s_mclk_pin(config[CONF_I2S_MCLK_PIN]))
    cg.add(var.set_pa_pin(config[CONF_PA_PIN]))
    cg.add(var.set_default_volume(config[CONF_DEFAULT_VOLUME]))

    if CONF_NOW_PLAYING in config:
        sens = await cg.get_variable(config[CONF_NOW_PLAYING])
        cg.add(var.set_now_playing_sensor(sens))

    if CONF_STATION_SELECT in config:
        sel = await cg.get_variable(config[CONF_STATION_SELECT])
        cg.add(var.set_station_select(sel))
