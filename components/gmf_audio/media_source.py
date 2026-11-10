import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import media_source, text_sensor
from esphome.const import CONF_ID
from . import gmf_audio_ns

AUTO_LOAD = ["audio", "text_sensor", "gmf_audio"]
GmfMediaSource = gmf_audio_ns.class_("GmfMediaSource", cg.Component, media_source.MediaSource)
CONF_TITLE = "title_id"
CONF_PHASE = "phase_id"

CONFIG_SCHEMA = cv.All(
    media_source.media_source_schema(GmfMediaSource).extend(
        {
            cv.Optional(CONF_TITLE): cv.use_id(text_sensor.TextSensor),
            cv.Optional(CONF_PHASE): cv.use_id(text_sensor.TextSensor),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework("esp-idf"),
)


async def to_code(config):
    cg.add_define("USE_GMF_MEDIA_SOURCE")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)
    if CONF_TITLE in config:
        cg.add(var.set_title_sensor(await cg.get_variable(config[CONF_TITLE])))
    if CONF_PHASE in config:
        cg.add(var.set_phase_sensor(await cg.get_variable(config[CONF_PHASE])))
