"""ESPHome media_player platform: internet_radio."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import media_player, esp32, text_sensor, select
from esphome.const import CONF_ID
from esphome.components.internet_radio import internet_radio_ns
from esphome.components.esp32 import add_idf_component
from esphome import pins

DEPENDENCIES = ["network", "i2c"]
AUTO_LOAD = ["gmf_audio"]

InternetRadio = internet_radio_ns.class_(
    "InternetRadio", media_player.MediaPlayer, cg.Component
)

CONF_I2S_BCLK_PIN = "i2s_bclk_pin"
CONF_I2S_LRCLK_PIN = "i2s_lrclk_pin"
CONF_I2S_DOUT_PIN = "i2s_dout_pin"
CONF_I2S_DIN_PIN = "i2s_din_pin"
CONF_I2S_MCLK_PIN = "i2s_mclk_pin"
CONF_PA_PIN = "pa_pin"
CONF_DEFAULT_VOLUME = "default_volume"
CONF_NOW_PLAYING = "now_playing_id"
CONF_STATION_SELECT = "station_select_id"
CONF_PLAYBACK_PHASE = "playback_phase_id"

CONFIG_SCHEMA = cv.All(
    media_player.media_player_schema(InternetRadio)
    .extend(
        {
            cv.Required(CONF_I2S_BCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_I2S_LRCLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_I2S_DOUT_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_I2S_DIN_PIN, default=16): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_I2S_MCLK_PIN, default=2): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_PA_PIN, default=-1): cv.Any(-1, pins.internal_gpio_output_pin_number),
            cv.Optional(CONF_DEFAULT_VOLUME, default=15): cv.int_range(min=0, max=21),
            cv.Optional(CONF_NOW_PLAYING): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_STATION_SELECT): cv.use_id(select.Select),
            cv.Optional(CONF_PLAYBACK_PHASE): cv.use_id(text_sensor.TextSensor),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_player.register_media_player(var, config)

    # ESP-IDF: un-exclude built-in IDF components
    esp32.include_builtin_idf_component("esp_driver_i2s")
    esp32.include_builtin_idf_component("esp_http_client")

    # ES8311 codec driver (register-level control via I2C).
    # 1.5.10 is compatible with the native ESP-IDF toolchain.
    add_idf_component(name="espressif/esp_codec_dev", ref="1.5.10")

    cg.add(var.set_i2s_bclk_pin(config[CONF_I2S_BCLK_PIN]))
    cg.add(var.set_i2s_lrclk_pin(config[CONF_I2S_LRCLK_PIN]))
    cg.add(var.set_i2s_dout_pin(config[CONF_I2S_DOUT_PIN]))
    cg.add(var.set_i2s_din_pin(config[CONF_I2S_DIN_PIN]))
    cg.add(var.set_i2s_mclk_pin(config[CONF_I2S_MCLK_PIN]))
    cg.add(var.set_pa_pin(config[CONF_PA_PIN]))
    cg.add(var.set_default_volume(config[CONF_DEFAULT_VOLUME]))

    if CONF_NOW_PLAYING in config:
        sens = await cg.get_variable(config[CONF_NOW_PLAYING])
        cg.add(var.set_now_playing_sensor(sens))

    if CONF_STATION_SELECT in config:
        sel = await cg.get_variable(config[CONF_STATION_SELECT])
        cg.add(var.set_station_select(sel))

    if CONF_PLAYBACK_PHASE in config:
        sensor = await cg.get_variable(config[CONF_PLAYBACK_PHASE])
        cg.add(var.set_playback_phase_sensor(sensor))
